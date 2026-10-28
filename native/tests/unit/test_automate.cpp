// PC5 I-0 (SPEC.md AUTO-01: 自動処理の土台): the steps of a script in their three forms (PhotoCraft v0.2.0's
// engine/src/automate_cmds/tests.rs parse_script, read as Genko's ops) and their errors with the line or the step; the ops
// no step may be (the gates, the page locks, undo — inside for_pages too) and an agent's ops only for an actor that cannot
// approve; the pseudo ops made ops on the book as it is (fit_image: its box, its shape kept, dont_enlarge) and the one
// still refused; the runner's inputs (a folder's pictures in order, hidden files and other kinds left out, a book's
// folder), its outputs' names (prefix, suffix, a book's page, the next free -2), a file that fails said and the next one
// taken (or the run stopped), cancelled between files, a picture through no steps written as コピーを保存 writes it; the
// オートアクション kept by service/actions (the same file and form, the gates never recorded); the script events' table
// sealed by the window, read turned off when changed outside, a script changed since it was bound not run, and no event
// raised while an event's steps run.

#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/codecs/codecs.hpp"
#include "service/actions.hpp"
#include "service/automate.hpp"
#include "service/script_events.hpp"
#include "storage/fsutil.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace automate = service::automate;
namespace events = service::script_events;
namespace fs = std::filesystem;

namespace {

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }

// What a call throws: its code and message (empty: nothing thrown).
template <typename F>
std::pair<std::string, std::string> thrown(F&& f) {
    try {
        f();
    } catch (const core::Error& error) {
        return {error.code(), error.what()};
    }
    return {};
}

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

// A white picture of w × h at 72 ppi as a PNG's bytes.
std::string png(int w, int h) {
    formats::NewImage image;
    image.title = "p";
    image.width = w;
    image.height = h;
    const formats::Opened made = formats::new_image(image);
    const fs::path dir = fs::temp_directory_path() / ("genko_automate_png_" + std::to_string(w) + "x" + std::to_string(h));
    fs::create_directories(dir);
    const fs::path file = dir / "p.png";
    formats::write_copy(made.doc, 0, file);
    return storage::read_file(file);
}

double page_px(const core::Document& doc, bool width) {
    const core::PageSpec& s = doc.page(0).spec;
    return (width ? s.width_mm.value() : s.height_mm.value()) / 25.4 * s.dpi.value();
}

}  // namespace

class TestAutomate : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QVERIFY(config_.isValid());
        qputenv("GENKO_CONFIG_DIR", config_.path().toUtf8());
    }

    // The three forms read alike: a JSON array, {"steps": […]}, one op a line (a BOM, notes and blank lines).
    void stepsInThreeForms() {
        const Json want = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "L1"}},
                                       Json{{"op", "set_note"}, {"page", 1}, {"note", "あ b"}}, Json{{"op", "for_pages"}}});
        QCOMPARE(automate::read_steps(want.dump()), want);
        QCOMPARE(automate::read_steps(Json{{"steps", want}}.dump()), want);
        const std::string lines =
            "\xEF\xBB\xBF# 影を足す\n\nadd_layer {\"page\": 1, \"kind\": \"paint\", \"id\": \"L1\"}\r\n  // メモ\nset_note {\"page\": 1, \"note\": \"あ b\"}\nfor_pages\n";
        QCOMPARE(automate::read_steps(lines), want);
        QCOMPARE(automate::read_steps(""), Json::array());
        QCOMPARE(automate::read_steps("# only a note\n"), Json::array());
        // the line or the step said
        QCOMPARE(thrown([] { automate::read_steps("set_note {\"page\": 1}\nset_note {\"page\": \n"); }).second.substr(0, 8), std::string("line 2: "));
        QCOMPARE(thrown([] { automate::read_steps("set_note [1]"); }).second, std::string("line 1: the arguments are one JSON object {…}"));
        QCOMPARE(thrown([] { automate::read_steps("\n\nSet-Note {}"); }).second.substr(0, 8), std::string("line 3: "));
        QCOMPARE(thrown([] { automate::read_steps("set_note {\"op\": \"approve\"}"); }).second, std::string("line 1: the op is named twice"));
        QCOMPARE(thrown([] { automate::read_steps("[{\"op\": \"set_note\"}, {\"page\": 1}]"); }).second.substr(0, 8), std::string("step 2: "));
        QCOMPARE(thrown([] { automate::read_steps("{\"ops\": []}"); }).first, std::string("steps"));
        QCOMPARE(thrown([] { automate::read_steps("[{\"op\": \"set_note\", \"x\": NaN}]"); }).first, std::string("steps"));
        QCOMPARE(thrown([] { automate::read_steps(std::string(automate::kMaxStepsBytes + 1, ' ')); }).second, std::string("the steps are larger than 1 MiB"));
        std::string many;
        for (std::size_t i = 0; i <= automate::kMaxSteps; ++i) many += "set_note\n";
        QCOMPARE(thrown([&] { automate::read_steps(many); }).second, std::string("more than 10000 steps"));
    }

    // No step approves or locks, nor undoes — anywhere —; an actor that cannot approve runs an agent's ops only.
    void stepsNeverGoRoundTheApprovals() {
        const core::Actor person("human:leaf");
        const core::Actor agent("ai:hermes");
        for (const char* gate : {"approve", "revoke", "allow_chat_approval", "lock_page", "unlock_page", "name_ok", "advance"}) {
            const Json steps = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}, Json{{"op", gate}, {"page", 1}}});
            const auto [code, message] = thrown([&] { automate::check_steps(steps, person); });
            QCOMPARE(code, std::string("steps"));
            QVERIFY2(message.rfind("step 2: " + std::string(gate) + " cannot be a step", 0) == 0, message.c_str());
            const Json inside = Json::array({Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", gate}}})}}});
            QVERIFY(thrown([&] { automate::check_steps(inside, person); }).second.rfind("step 1.1: ", 0) == 0);
            QVERIFY(!service::actions::recordable(steps).empty());
            QCOMPARE(service::actions::recordable(steps).size(), std::size_t(1));  // (never recorded either)
        }
        QVERIFY(thrown([&] { automate::check_steps(Json::array({Json{{"op", "undo"}}}), person); }).second.rfind("step 1: undo is not a step", 0) == 0);
        // an agent: apply_ops' ops only (resize_pages is not one), for_pages' too
        const Json mine = Json::array({Json{{"op", "add_layer"}, {"page", 1}}, Json{{"op", "resize_pages"}, {"page", 1}, {"width_px", 10}}});
        automate::check_steps(mine, person);
        QVERIFY(thrown([&] { automate::check_steps(mine, agent); }).second.rfind("step 2: resize_pages is not an op an agent may run", 0) == 0);
        const Json nested = Json::array({Json{{"op", "for_pages"}, {"ops", Json::array({Json{{"op", "resize_pages"}}})}}});
        QVERIFY(thrown([&] { automate::check_steps(nested, agent); }).second.rfind("step 1.1: resize_pages", 0) == 0);
        QVERIFY(thrown([&] { automate::check_steps(Json::array({Json{{"op", "set_note"}}}), core::Actor("legacy:unknown")); }).first.empty());
        // a pseudo op is checked by what it stands for: fit_image is resize_pages, refused for an agent when it is run
        const core::Document doc = formats::new_image(formats::NewImage{"p", 200, 100}).doc;
        const Json fit = Json::array({Json{{"op", "fit_image"}, {"page", 1}, {"width_px", 100}, {"height_px", 100}}});
        automate::check_steps(fit, agent);
        const auto [code, message] = thrown([&] { automate::run_steps(doc, fit, agent); });
        QCOMPARE(code, std::string("steps"));
        QVERIFY2(message.rfind("step 1 (fit_image): step 1: resize_pages is not an op an agent may run", 0) == 0, message.c_str());
    }

    // Pseudo ops: made ops on the book as it is when their turn comes.
    void pseudoOpsBecomeOps() {
        QVERIFY(automate::pseudo_op("fit_image"));
        QVERIFY(automate::pseudo_op("conditional_mode_change"));
        QVERIFY(!automate::pseudo_op("resize_pages"));
        const core::Document doc = formats::new_image(formats::NewImage{"p", 200, 100}).doc;
        const Json fit{{"op", "fit_image"}, {"page", 1}, {"width_px", 100}, {"height_px", 100}};
        QCOMPARE(automate::expand_pseudo(doc, fit),
                 Json::array({Json{{"op", "resize_pages"}, {"page", 1}, {"width_px", 50 * 2}, {"height_px", 50}, {"resample", "bicubic"}}}));
        Json bigger = fit;
        bigger["width_px"] = 400;
        bigger["height_px"] = 400;
        bigger["dont_enlarge"] = true;
        QCOMPARE(automate::expand_pseudo(doc, bigger), Json::array());  // (it would grow)
        bigger.erase("dont_enlarge");
        QCOMPARE(automate::expand_pseudo(doc, bigger)[0]["width_px"], Json(400));
        QCOMPARE(automate::expand_pseudo(doc, Json{{"op", "fit_image"}, {"page", 1}, {"width_px", 200}, {"height_px", 100}}), Json::array());
        for (const Json& wrong : {Json{{"op", "fit_image"}, {"page", 1}, {"width_px", 0}}, Json{{"op", "fit_image"}, {"page", 7}},
                                  Json{{"op", "fit_image"}, {"width_px", 5}}, Json{{"op", "fit_image"}, {"page", 1}, {"dont_enlarge", "yes"}}}) {
            bool refused = false;
            try {
                automate::expand_pseudo(doc, wrong);
            } catch (const core::OpError&) {
                refused = true;
            }
            QVERIFY2(refused, wrong.dump().c_str());
        }
        // run: the ops before it first, then it on the book as they left it; one result (its journal the ops as run)
        const Json steps = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "a"}}, fit,
                                        Json{{"op", "set_note"}, {"page", 1}, {"note", "b"}}});
        const core::ApplyResult ran = automate::run_steps(doc, steps, core::Actor("human:leaf"));
        QCOMPARE(page_px(ran.doc, true), 100.0);
        QCOMPARE(page_px(ran.doc, false), 50.0);
        QCOMPARE(ran.doc.page(0).note, std::string("b"));
        QCOMPARE(ran.applied, Json::array({"set_note", "resize_pages", "set_note"}));
        QCOMPARE(ran.journal_ops.size(), std::size_t(3));
        QCOMPARE(ran.journal_ops[1]["op"], Json("resize_pages"));
        QCOMPARE(page_px(doc, true), 200.0);  // (the book given unchanged)
        // no pseudo op: the bus's own result, as `genko apply` has it
        const Json plain = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "a"}}});
        QCOMPARE(automate::run_steps(doc, plain, core::Actor("human:leaf")).applied, Json::array({"set_note"}));
        // the one PC5 I-e fills in
        const auto [code, message] = thrown([&] {
            automate::run_steps(doc, Json::array({Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "gray"}}}), core::Actor("human:leaf"));
        });
        QCOMPARE(code, std::string("not_yet_ported"));
        QCOMPARE(message, std::string("ops[0] conditional_mode_change: conditional_mode_change is not in the C++ build yet"));
    }

    // The steps aimed at the pages: each page of a picture, a book's pages but the covers; an action played on each.
    void stepsAimedAtThePages() {
        const core::Document doc = formats::new_image(formats::NewImage{"p", 20, 10}).doc;
        const Json aimed = automate::aim_steps(doc, Json::array({Json{{"op", "set_note"}, {"page", 9}, {"note", "x"}}}), true, false);
        QCOMPARE(aimed, Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}}));
        // an action: its layer the picture's top one, a new id for what it makes
        const Json recorded = Json::array({Json{{"op", "add_layer"}, {"page", 3}, {"kind", "paint"}, {"id", "made"}},
                                           Json{{"op", "set_layer"}, {"page", 3}, {"id", "old"}, {"opacity", 0.5}},
                                           Json{{"op", "edit_line"}, {"id", "L9"}, {"text", "x"}}});
        const Json played = automate::aim_steps(doc, recorded, true, true);
        QCOMPARE(played.size(), std::size_t(2));  // (the line of the page it was recorded on left out)
        QCOMPARE(played[0]["page"], Json(1));
        QVERIFY(played[0]["id"] != Json("made"));
        QCOMPARE(played[1]["id"], Json(doc.page(0).layers.back().id));
        QCOMPARE(automate::aim_steps(doc, Json::array(), true, false), Json::array());
        QCOMPARE(thrown([&] { automate::aim_steps(doc, Json::array({Json{{"op", "add_page"}}}), true, false); }).first, std::string("steps"));
    }

    // The inputs: a folder's pictures by name (hidden files and other kinds left out, inner folders with recurse), a book.
    void inputsListed() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "in" / "sub");
        fs::create_directories(root / "in" / "book.genko");
        write(root / "in" / "book.genko" / "project.json", "{}");
        for (const char* name : {"b.png", "a.JPG", ".hidden.png", "notes.txt", "sub/c.tif"}) write(root / "in" / name, "x");
        automate::Inputs got = automate::list_inputs({root / "in", root / "gone.png"});
        QCOMPARE(got.items.size(), std::size_t(3));
        QCOMPARE(got.items[0].path.filename().string(), std::string("a.JPG"));
        QCOMPARE(got.items[1].path.filename().string(), std::string("b.png"));
        QVERIFY(got.items[2].book);  // (book.genko: a document of the folder)
        QVERIFY(!got.items[1].book);
        QCOMPARE(got.skipped.size(), std::size_t(1));
        QCOMPARE(got.skipped[0]["reason"], Json("not found"));
        automate::InputOptions deep;
        deep.recurse = true;
        got = automate::list_inputs({root / "in"}, deep);
        QCOMPARE(got.items.size(), std::size_t(4));
        QCOMPARE(got.items[3].path.filename().string(), std::string("c.tif"));
        QCOMPARE(automate::list_inputs({root / "in" / "book.genko", root / "in" / "notes.txt"}).items.size(), std::size_t(2));
        deep.limit = 3;
        QCOMPARE(thrown([&] { automate::list_inputs({root / "in"}, deep); }).first, std::string("value"));
        for (const char* ext : {"png", "jpg", "tif", "psd", "psb", "pcraft", "dng"}) {
            const auto& all = automate::input_extensions();
            QVERIFY2(std::find(all.begin(), all.end(), std::string(ext)) != all.end(), ext);
        }
    }

    // The outputs' names: prefix, suffix, a book's page, a name every OS takes, the next free -2.
    void outputsNamed() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        automate::Output out;
        out.folder = root;
        out.prefix = "x_";
        out.suffix = "_s";
        const automate::Input picture{root / "src" / "写真.JPEG", false};
        QCOMPARE(automate::output_extension(out, picture), std::string("jpg"));
        QCOMPARE(automate::output_extension(out, automate::Input{root / "a.cr2", false}), std::string("png"));
        QCOMPARE(automate::output_extension(out, automate::Input{root / "b.genko", true}), std::string("png"));
        out.format = "TIFF";
        QCOMPARE(automate::output_extension(out, picture), std::string("tif"));
        QCOMPARE(automate::output_path(out, picture, {}, "jpg"), root / core::path_from_utf8("x_写真_s.jpg"));
        write(root / core::path_from_utf8("x_写真_s.jpg"), "x");
        QCOMPARE(automate::output_path(out, picture, {}, "jpg"), root / core::path_from_utf8("x_写真_s-2.jpg"));
        QCOMPARE(automate::output_path(out, picture, {}, "jpg", {root / core::path_from_utf8("x_写真_s-2.jpg")}), root / core::path_from_utf8("x_写真_s-3.jpg"));
        out.prefix.clear();
        out.suffix.clear();
        QCOMPARE(automate::output_path(out, automate::Input{root / "作品.genko", true}, "p003", "png"), root / core::path_from_utf8("作品_p003.png"));
        QCOMPARE(automate::output_path(out, automate::Input{root / "a:b?.png", false}, {}, "png").filename().string().find_first_of(":?"), std::string::npos);
    }

    // The runner: a picture through no steps written as コピーを保存 writes it; a file that fails said and the next taken, or
    // the run stopped; cancelled between files; a picture's own file never written.
    void runnerTakesFilesOneByOne() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "in");
        write(root / "in" / "a.png", png(30, 20));
        write(root / "in" / "b.png", "not a picture");
        write(root / "in" / "c.png", png(8, 8));
        const automate::Inputs inputs = automate::list_inputs({root / "in"});
        QCOMPARE(inputs.items.size(), std::size_t(3));
        automate::Output out;
        out.folder = root / "out";
        automate::Options options;
        options.actor = core::Actor("human:leaf");
        std::vector<std::size_t> progress;
        options.progress = [&](std::size_t done, std::size_t, const fs::path&) { progress.push_back(done); };
        automate::Result result = automate::run(inputs.items, Json::array(), out, options);
        QCOMPARE(result.files.size(), std::size_t(2));
        QCOMPARE(result.errors.size(), std::size_t(1));
        QCOMPARE(result.errors[0]["input"], Json(core::path_to_utf8(root / "in" / "b.png")));
        QCOMPARE(result.json()["ok"], Json(false));
        QCOMPARE(progress, (std::vector<std::size_t>{0, 1, 2, 3}));
        // the same bytes コピーを保存 writes of the picture opened
        const formats::Opened opened = formats::open_file(storage::read_file(root / "in" / "a.png"), "a.png");
        formats::write_copy(opened.doc, 0, root / "copy.png");
        QCOMPARE(storage::read_file(root / "out" / "a.png"), storage::read_file(root / "copy.png"));
        // again: the next free names, never over the files there
        result = automate::run(inputs.items, Json::array(), out, options);
        QCOMPARE(result.files[0]["output"], Json(core::path_to_utf8(root / "out" / "a-2.png")));
        // stop at the first error
        options.stop_on_error = true;
        result = automate::run(inputs.items, Json::array(), out, options);
        QCOMPARE(result.files.size(), std::size_t(1));
        QCOMPARE(result.errors.size(), std::size_t(1));
        // cancelled after the first file
        options.stop_on_error = false;
        int asked = 0;
        options.cancelled = [&asked] { return ++asked > 1; };
        result = automate::run(inputs.items, Json::array(), out, options);
        QVERIFY(result.cancelled);
        QCOMPARE(result.files.size(), std::size_t(1));
        QCOMPARE(result.json()["cancelled"], Json(true));
        // steps that cannot run: nothing opened
        options.cancelled = {};
        QCOMPARE(thrown([&] { automate::run(inputs.items, Json::array({Json{{"op", "approve"}}}), out, options); }).first, std::string("steps"));
        // a picture's own file is never written
        automate::Output in_place;
        in_place.to = automate::Output::To::Book;
        result = automate::run({inputs.items[0]}, Json::array(), in_place, options);
        QCOMPARE(result.errors[0]["code"], Json("refused_target"));
        // steps run on each picture: its note set, then written
        automate::Output none;
        none.to = automate::Output::To::None;
        result = automate::run({inputs.items[0]}, Json::array({Json{{"op", "fit_image"}, {"width_px", 15}, {"height_px", 15}}}), none, options);
        QCOMPARE(result.errors.size(), std::size_t(0));
        QCOMPARE(result.files.size(), std::size_t(1));
        out.format = "jpg";
        out.quality = 13;
        QCOMPARE(thrown([&] { automate::run(inputs.items, Json::array(), out, options); }).second, std::string("the quality is 0 to 12"));
    }

    // オートアクション kept by service/actions: actions.json in the settings folder, as before.
    void actionsKeptInTheSettingsFolder() {
        QCOMPARE(service::actions::path(), path_of(config_.path()) / "actions.json");
        QVERIFY(!service::actions::store("影", Json::array({Json{{"op", "add_layer"}, {"page", 1}}}), "2026-10-28 12:00"));
        QCOMPARE(storage::read_file(service::actions::path()),
                 std::string("{\n \"影\": {\n  \"ops\": [\n   {\n    \"op\": \"add_layer\",\n    \"page\": 1\n   }\n  ],\n  \"made\": \"2026-10-28 12:00\"\n }\n}"));
        QCOMPARE(service::actions::load()["影"]["made"], Json("2026-10-28 12:00"));
        QVERIFY(!service::actions::remove("影"));
        QVERIFY(service::actions::load().empty());
    }

    // The script events' table: sealed by the window; changed outside, read off with why; a script changed since it was
    // bound not run; everything's bindings with each event's; no event while an event's steps run.
    void scriptEventsSealed() {
        QVERIFY(!events::load().problem);
        QVERIFY(events::load().table.bindings.empty());
        QCOMPARE(events::path(), path_of(config_.path()) / "script_events.json");
        for (const events::Event e : events::events()) QCOMPARE(events::event_of(events::id_of(e)), std::optional<events::Event>(e));
        QCOMPARE(events::events().size(), std::size_t(8));
        const fs::path script = path_of(config_.path()) / "note.txt";
        write(script, "set_note {\"page\": 1, \"note\": \"saved\"}\n");
        events::Table table;
        table.enabled = true;
        table.bindings.push_back({"saveDocument", "メモ", core::path_to_utf8(script), storage::sha256_hex(storage::read_file(script)), std::nullopt});
        table.bindings.push_back({"everything", "影", std::nullopt, std::nullopt, std::string("影")});
        QVERIFY(!events::save(table));
        events::Loaded loaded = events::load();
        QVERIFY(!loaded.problem);
        QVERIFY(loaded.table.enabled);
        QCOMPARE(events::to_json(loaded.table), events::to_json(table));
        QCOMPARE(events::bound_to(loaded.table, events::Event::SaveDocument).size(), std::size_t(2));
        QCOMPARE(events::bound_to(loaded.table, events::Event::Print).size(), std::size_t(1));
        const events::Steps steps = events::steps_of(loaded.table.bindings[0], events::Event::SaveDocument);
        QCOMPARE(steps.steps, Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "saved"}}}));
        QCOMPARE(steps.label, std::string("メモ（ドキュメントを保存）"));
        QVERIFY(!steps.recorded);
        QCOMPARE(thrown([&] { events::steps_of(loaded.table.bindings[1], events::Event::Print); }).first, std::string("not_found"));  // (no such action)
        // the script changed since it was bound: not run
        write(script, "set_note {\"page\": 1, \"note\": \"other\"}\n");
        QCOMPARE(thrown([&] { events::steps_of(loaded.table.bindings[0], events::Event::SaveDocument); }).first, std::string("script_changed"));
        // the table changed outside Genko: off, and said
        std::string bytes = storage::read_file(events::path());
        write(events::path(), bytes + " ");
        loaded = events::load();
        QVERIFY(loaded.problem.has_value());
        QVERIFY(!loaded.table.enabled);
        QCOMPARE(loaded.table.bindings.size(), std::size_t(2));
        QVERIFY(events::bound_to(loaded.table, events::Event::SaveDocument).empty());
        write(events::path(), "{broken");
        QVERIFY(events::load().problem.has_value());
        QVERIFY(!events::load().table.enabled);
        // a table that is not one
        QCOMPARE(thrown([] { events::from_json(Json{{"bindings", Json::array({Json{{"event", "boot"}, {"action", "x"}}})}}); }).first, std::string("value"));
        QCOMPARE(thrown([] { events::from_json(Json{{"bindings", Json::array({Json{{"event", "print"}, {"script", "/a"}}})}}); }).first, std::string("value"));
        // no event while an event's steps run
        QVERIFY(!events::Firing::active());
        {
            const events::Firing outer;
            QVERIFY(outer.first());
            QVERIFY(events::Firing::active());
            const events::Firing inner;
            QVERIFY(!inner.first());
        }
        QVERIFY(!events::Firing::active());
    }

private:
    QTemporaryDir config_;
};

QTEST_GUILESS_MAIN(TestAutomate)
#include "test_automate.moc"
