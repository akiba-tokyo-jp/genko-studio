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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/codecs/codecs.hpp"
#include "render/ops_registry.hpp"
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

// A white picture's book of w × h at 72 ppi (新規… → 画像).
core::Document picture(int w, int h) {
    formats::NewImage image;
    image.title = "p";
    image.width = w;
    image.height = h;
    return formats::new_image(image).doc;
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
        QCOMPARE(thrown([] { automate::read_steps("set_note {\"page\": 1}\nset_note {\"page\": \n"); }).second.substr(0, 15), std::string("steps, line 2: "));
        QCOMPARE(thrown([] { automate::read_steps("set_note [1]"); }).second, std::string("steps, line 1: the arguments are one JSON object {…}"));
        QCOMPARE(thrown([] { automate::read_steps("\n\nSet-Note {}"); }).second.substr(0, 15), std::string("steps, line 3: "));
        QCOMPARE(thrown([] { automate::read_steps("set_note {\"op\": \"approve\"}"); }).second, std::string("steps, line 1: the op is named twice"));
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
        // a pseudo op is made ops on the steps' own level only: inside for_pages it is refused when checked
        const Json repeated = Json::array({Json{{"op", "for_pages"}, {"ops", Json::array({Json{{"op", "fit_image"}, {"width_px", 5}}})}}});
        QCOMPARE(thrown([&] { automate::check_steps(repeated, person); }).second,
                 std::string("step 1.1: fit_image is a pseudo op and cannot be inside for_pages"));
        // the journal's name of a script: 200 characters at most
        automate::check_script_name(QString(200, QChar(0x3042)).toStdString());
        QCOMPARE(thrown([] { automate::check_script_name(QString(201, QChar(0x3042)).toStdString()); }).first, std::string("value"));
        // an agent: apply_ops' ops only (resize_pages is not one), for_pages' too
        const Json mine = Json::array({Json{{"op", "add_layer"}, {"page", 1}}, Json{{"op", "resize_pages"}, {"page", 1}, {"width_px", 10}}});
        automate::check_steps(mine, person);
        QVERIFY(thrown([&] { automate::check_steps(mine, agent); }).second.rfind("step 2: resize_pages is not an op an agent may run", 0) == 0);
        const Json nested = Json::array({Json{{"op", "for_pages"}, {"ops", Json::array({Json{{"op", "resize_pages"}}})}}});
        QVERIFY(thrown([&] { automate::check_steps(nested, agent); }).second.rfind("step 1.1: resize_pages", 0) == 0);
        QVERIFY(thrown([&] { automate::check_steps(Json::array({Json{{"op", "set_note"}}}), core::Actor("legacy:unknown")); }).first.empty());
        // a pseudo op is checked by what it stands for: fit_image is resize_pages, refused for an agent when it is run
        const core::Document doc = picture(200, 100);
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
        const core::Document doc = picture(200, 100);
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
        QCOMPARE(std::round(page_px(ran.doc, true)), 100.0);  // (resize_pages keeps the paper in mm)
        QCOMPARE(std::round(page_px(ran.doc, false)), 50.0);
        QCOMPARE(ran.doc.page(0).note, std::string("b"));
        QCOMPARE(ran.applied, Json::array({"set_note", "resize_pages", "set_note"}));
        QCOMPARE(ran.journal_ops.size(), std::size_t(3));
        QCOMPARE(ran.journal_ops[1]["op"], Json("resize_pages"));
        QCOMPARE(page_px(doc, true), 200.0);  // (the book given unchanged)
        // no pseudo op: the bus's own result, as `genko apply` has it
        const Json plain = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "a"}}});
        QCOMPARE(automate::run_steps(doc, plain, core::Actor("human:leaf")).applied, Json::array({"set_note"}));
        // (PC5 I-e: 条件付きモード変更 — what was refused with not_yet_ported until then — made set_color_mode)
        const core::ApplyResult grey =
            automate::run_steps(doc, Json::array({Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "gray"}}}), core::Actor("human:leaf"));
        QCOMPARE(grey.applied, Json::array({"set_color_mode"}));
        QCOMPARE(automate::page_mode(grey.doc.page(0)), std::string("gray"));
    }

    // PC5 I-e: 条件付きモード変更 (PhotoCraft's file_cmds.rs conditional_mode_change): the page into `to` when its mode is one
    // of `from` (any: all; grayscale is gray), its depth kept; nothing when it is not, or is `to` already; the modes named
    // as Photoshop's list (bitmap … multichannel).
    void conditionalModeChange() {
        const core::Document doc = picture(20, 10);
        QCOMPARE(automate::page_mode(doc.page(0)), std::string("rgb"));
        const auto change = [&doc](Json from, const char* to) {
            Json op{{"op", "conditional_mode_change"}, {"page", 1}, {"to", to}};
            if (!from.is_null()) op["from"] = std::move(from);
            return automate::expand_pseudo(doc, op);
        };
        QCOMPARE(change(Json::array({"rgb", "cmyk"}), "gray"), Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}}}));
        QCOMPARE(change(Json::array({"Grayscale", "cmyk"}), "lab"), Json::array());  // (an RGB page: not one of them)
        QCOMPARE(change(Json(), "rgb"), Json::array());                               // (any: but it is RGB already)
        QCOMPARE(change(Json("any"), "cmyk")[0]["mode"], Json("cmyk"));
        QCOMPARE(change(Json::array({"rgb"}), "grayscale")[0]["mode"], Json("gray"));
        // its depth kept: a grey page of 16 bits into RGB of 16 bits
        const core::ApplyResult grey16 = core::CommandBus(render::ops_registry())
                                             .apply(doc, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "gray"}, {"depth", 16}}}),
                                                    core::Actor("human:leaf"));
        QCOMPARE(automate::page_mode(grey16.doc.page(0)), std::string("gray"));
        const Json to_rgb = automate::expand_pseudo(grey16.doc, Json{{"op", "conditional_mode_change"}, {"page", 1}, {"from", Json::array({"gray"})}, {"to", "rgb"}});
        QCOMPARE(to_rgb, Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}}}));
        const core::ApplyResult made = automate::run_steps(grey16.doc, Json::array({Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "rgb"}}}),
                                                           core::Actor("human:leaf"));
        QCOMPARE(automate::page_mode(made.doc.page(0)), std::string("rgb"));
        QVERIFY(made.doc.page(0).rgb && made.doc.page(0).rgb->depth == 16);
        for (const Json& wrong : {Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "indexed"}},
                                  Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "rgb"}, {"from", Json::array({"sepia"})}},
                                  Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "rgb"}, {"from", 3}},
                                  Json{{"op", "conditional_mode_change"}, {"page", 1}}, Json{{"op", "conditional_mode_change"}, {"to", "rgb"}}}) {
            bool refused = false;
            try {
                automate::expand_pseudo(doc, wrong);
            } catch (const core::OpError&) {
                refused = true;
            }
            QVERIFY2(refused, wrong.dump().c_str());
        }
        // an agent: set_color_mode is not one of its ops, so the change it stands for is refused
        const auto [code, message] = thrown([&] {
            automate::run_steps(doc, Json::array({Json{{"op", "conditional_mode_change"}, {"page", 1}, {"to", "gray"}}}), core::Actor("ai:hermes"));
        });
        QCOMPARE(code, std::string("steps"));
        QVERIFY2(message.find("set_color_mode is not an op an agent may run") != std::string::npos, message.c_str());
    }

    // PC5 I-e: 画像プロセッサー's outputs (a folder each format, Photoshop's names; JPEG's quality only) and its steps (sRGB:
    // a mode convert_profile does not take made RGB first; サイズに合わせる never enlarged), レンズ補正's (flattened first
    // when more than one layer shows, then the filter on the layer kept, the profile auto unless given).
    void processorAndLensSteps() {
        automate::ImageProcessor processor;
        processor.formats = {"jpeg", "PSD", "tif"};
        processor.quality = 5;
        const fs::path out = fs::path("out");
        const automate::Output output = automate::processor_output(processor, out);
        QCOMPARE(output.folder, out / "JPEG");
        QCOMPARE(output.format, std::string("jpg"));
        QCOMPARE(output.quality, std::optional<int>(5));
        QCOMPARE(output.also.size(), std::size_t(2));
        QCOMPARE(output.also[0].folder, out / "PSD");
        QCOMPARE(output.also[0].format, std::string("psd"));
        QVERIFY(!output.also[0].quality);
        QCOMPARE(output.also[1].folder, out / "TIFF");
        for (const std::vector<std::string>& wrong : {std::vector<std::string>{}, {"jpg", "jpeg"}, {"gif"}}) {
            automate::ImageProcessor bad;
            bad.formats = wrong;
            QCOMPARE(thrown([&] { automate::processor_output(bad, out); }).first, std::string("value"));
        }
        const core::Document doc = picture(40, 20);
        QCOMPARE(automate::processor_steps(processor, doc), Json::array());
        processor.to_srgb = true;
        processor.width_px = 10;
        const Json steps = automate::processor_steps(processor, doc);
        QCOMPARE(steps.size(), std::size_t(3));
        QCOMPARE(steps[0]["op"], Json("conditional_mode_change"));
        QCOMPARE(steps[1], (Json{{"op", "convert_profile"}, {"page", 1}, {"profile", "srgb"}}));
        QCOMPARE(steps[2], (Json{{"op", "fit_image"}, {"page", 1}, {"dont_enlarge", true}, {"width_px", 10}}));
        const core::ApplyResult ran = automate::run_steps(doc, steps, core::Actor("human:leaf"));
        QCOMPARE(std::round(page_px(ran.doc, true)), 10.0);
        QCOMPARE(std::round(page_px(ran.doc, false)), 5.0);
        // レンズ補正: one layer — the filter on it; two showing — flattened first, the lowest kept
        const Json lens = automate::lens_steps(Json{{"profile", "generic"}, {"focal_length", 24}}, doc);
        QCOMPARE(lens.size(), std::size_t(1));
        QCOMPARE(lens[0]["op"], Json("filter_raster"));
        QCOMPARE(lens[0]["kind"], Json("lens_correction"));
        QCOMPARE(lens[0]["id"], Json(doc.page(0).layers[0].id));
        QCOMPARE(lens[0]["profile"], Json("generic"));
        QCOMPARE(automate::lens_steps(Json::object(), doc)[0]["profile"], Json("auto"));
        const core::ApplyResult two = core::CommandBus(render::ops_registry())
                                          .apply(doc, Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "L2"}}}),
                                                 core::Actor("human:leaf"));
        const Json both = automate::lens_steps(Json::object(), two.doc);
        QCOMPARE(both.size(), std::size_t(2));
        QCOMPARE(both[0], (Json{{"op", "merge_visible"}, {"page", 1}, {"copy", false}, {"flatten", true}}));
        QCOMPARE(both[1]["id"], Json(doc.page(0).layers[0].id));
        QCOMPARE(thrown([&] { automate::lens_steps(Json{{"page", 2}}, doc); }).first, std::string("value"));
        // the generic profile drawn on the picture through the runner's steps
        const core::ApplyResult corrected = automate::run_steps(doc, lens, core::Actor("human:leaf"));
        QCOMPARE(corrected.applied, Json::array({"filter_raster"}));
    }

    // PC5 I-e: a droplet — its file (written as it is read; what is not a droplet refused), its launchers (beside it, this
    // OS's, one fixed line calling `genko droplet`, runnable; a file of that name not a launcher never written over), its
    // output (given, its own, else droplet-output beside the first input); the runner's 連番 and 画像プロセッサー's Also.
    void droplets() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        automate::Droplet droplet;
        droplet.name = "影";
        droplet.steps = Json::array({Json{{"op", "set_note"}, {"note", "d"}}});
        droplet.format = "jpg";
        droplet.quality = 7;
        droplet.suffix = "_d";
        const fs::path written = automate::write_droplet(root / "影", droplet);
        QCOMPARE(written, root / "影.genkodroplet");
        const automate::Droplet back = automate::read_droplet(written);
        QCOMPARE(automate::droplet_json(back), automate::droplet_json(droplet));
        QCOMPARE(automate::droplet_json(droplet)["genkoDroplet"], Json(1));
        for (const Json& wrong :
             {Json{{"genkoDroplet", 2}, {"steps", Json::array()}}, Json{{"genkoDroplet", 1}}, Json{{"genkoDroplet", 1}, {"steps", Json::array()}, {"command", "rm"}},
              Json{{"genkoDroplet", 1}, {"steps", Json::array()}, {"options", Json{{"exec", "x"}}}},
              Json{{"genkoDroplet", 1}, {"steps", Json::array()}, {"options", Json{{"quality", 13}}}},
              Json{{"genkoDroplet", 1}, {"steps", Json::array()}, {"options", Json{{"format", "exe"}}}}}) {
            QVERIFY2(!thrown([&] { automate::droplet_from_json(wrong); }).first.empty(), wrong.dump().c_str());
        }
        QCOMPARE(thrown([&] { automate::droplet_from_json(Json{{"genkoDroplet", 1}, {"steps", Json::array({Json{{"x", 1}}})}}); }).first, std::string("steps"));
        write(root / "big.genkodroplet", std::string(automate::kMaxStepsBytes + 1, ' '));
        QCOMPARE(thrown([&] { automate::read_droplet(root / "big.genkodroplet"); }).first, std::string("value"));
        // never inside a book's folder
        fs::create_directories(root / "b.genko");
        write(root / "b.genko" / "project.json", "{}");
        QCOMPARE(thrown([&] { automate::write_droplet(root / "b.genko" / "x", droplet); }).first, std::string("refused_target"));
        QCOMPARE(thrown([&] { automate::write_launchers(root / "b.genko" / "x.genkodroplet", "/bin/genko"); }).first, std::string("refused_target"));
        // its launchers: one fixed line, quoted
        const fs::path odd = root / "it's $x.genkodroplet";
        QCOMPARE(automate::launcher_path(odd, automate::Launcher::Shell), root / "it's $x.sh");
        QCOMPARE(automate::launcher_text(odd, "/opt/g e/genko", automate::Launcher::Shell),
                 "#!/bin/sh\n# Genko droplet: runs the droplet on the files given (or dropped on it)\nexec '/opt/g e/genko' droplet '" +
                     core::path_to_utf8(root) + "/it'\\''s $x.genkodroplet' \"$@\"\n");
        const std::string desktop = automate::launcher_text(odd, "/opt/g e/genko", automate::Launcher::Desktop);
        QVERIFY2(desktop.find("Exec=\"/opt/g e/genko\" droplet \"" + core::path_to_utf8(root) + "/it's \\\\$x.genkodroplet\" %F\n") != std::string::npos, desktop.c_str());
        // Windows: a shortcut (MS-SHLLINK) of genko.exe with the argument droplet "<the droplet>" — no .cmd, no cmd.exe reading
        // the names dropped (the review's P2-2); read back as Windows reads it
        {
            const std::string link = automate::launcher_text(fs::path("C:/d/50% & 絵.genkodroplet"), "C:/g/genko.exe", automate::Launcher::Shortcut);
            QCOMPARE(automate::launcher_path(fs::path("C:/d/x.genkodroplet"), automate::Launcher::Shortcut), fs::path("C:/d/x.lnk"));
            const auto u32 = [&link](std::size_t at) {
                return std::uint32_t(std::uint8_t(link[at])) | std::uint32_t(std::uint8_t(link[at + 1])) << 8 | std::uint32_t(std::uint8_t(link[at + 2])) << 16 |
                       std::uint32_t(std::uint8_t(link[at + 3])) << 24;
            };
            const auto u16 = [&link](std::size_t at) { return std::uint16_t(std::uint8_t(link[at]) | std::uint8_t(link[at + 1]) << 8); };
            const auto text16 = [&link, &u16](std::size_t at, std::size_t n) {
                std::u16string out;
                for (std::size_t i = 0; i < n; ++i) out += char16_t(u16(at + 2 * i));
                return QString::fromStdU16String(out).toStdString();
            };
            QCOMPARE(u32(0), std::uint32_t(0x4C));
            QCOMPARE(link.substr(4, 16), std::string("\x01\x14\x02\x00\x00\x00\x00\x00\xC0\x00\x00\x00\x00\x00\x00\x46", 16));
            QCOMPARE(u32(20), std::uint32_t(0x02 | 0x04 | 0x20 | 0x80));  // (HasLinkInfo, HasName, HasArguments, IsUnicode)
            const std::size_t info = 0x4C;
            const std::uint32_t info_size = u32(info);
            QCOMPARE(u32(info + 4), std::uint32_t(0x24));
            QCOMPARE(u32(info + 8), std::uint32_t(1));
            const std::size_t base16 = info + u32(info + 28);
            std::size_t n = 0;
            while (u16(base16 + 2 * n) != 0) ++n;
            QCOMPARE(text16(base16, n), std::string("C:/g/genko.exe"));
            std::size_t at = info + info_size;
            const std::size_t name_n = u16(at);
            QCOMPARE(text16(at + 2, name_n), std::string("Genko droplet: drop files on it"));
            at += 2 + 2 * name_n;
            const std::size_t args_n = u16(at);
            QCOMPARE(text16(at + 2, args_n), std::string("droplet \"C:/d/50% & 絵.genkodroplet\""));
            at += 2 + 2 * args_n;
            QCOMPARE(u32(at), std::uint32_t(0));  // (TerminalBlock)
            QCOMPARE(link.size(), at + 4);
        }
        // a control character in any path a launcher names — a line end that would start a line of its own: refused for
        // every kind (the review's P2-1)
        for (const automate::Launcher kind : {automate::Launcher::Shell, automate::Launcher::Desktop, automate::Launcher::Shortcut}) {
            for (const char* bad : {"\n", "\r", "\t", "\x7f"}) {
                const fs::path drop = root / core::path_from_utf8(std::string("x") + bad + "Exec=sh -c evil.genkodroplet");
                QCOMPARE(thrown([&] { automate::launcher_text(drop, "/opt/genko", kind); }).first, std::string("refused_target"));
                QCOMPARE(thrown([&] { automate::launcher_text(root / "x.genkodroplet", std::string("/opt/ge") + bad + "nko", kind); }).first,
                         std::string("refused_target"));
            }
        }
        QCOMPARE(thrown([&] { automate::write_launchers(root / "a\nb.genkodroplet", "/opt/genko"); }).first, std::string("refused_target"));
        QVERIFY(!fs::exists(root / "a\nb.sh"));
        // $APPIMAGE: the launcher runs the AppImage, not its passing mount (the review's P3-3)
        QCOMPARE(automate::launcher_program("/tmp/.mount_x/usr/bin/genko"), fs::path("/tmp/.mount_x/usr/bin/genko"));
        write(root / "Genko.AppImage", "x");
        qputenv("APPIMAGE", core::path_to_utf8(root / "Genko.AppImage").c_str());
        QCOMPARE(automate::launcher_program("/tmp/.mount_x/usr/bin/genko"), root / "Genko.AppImage");
        qunsetenv("APPIMAGE");
        const std::vector<fs::path> made = automate::write_launchers(written, "/opt/genko");
        QCOMPARE(made.size(), automate::launchers_here().size());
        for (const fs::path& launcher : made) {
            QCOMPARE(launcher.parent_path(), root);
            QVERIFY(storage::read_file(launcher).find("droplet") != std::string::npos);
#ifndef _WIN32
            QVERIFY((fs::status(launcher).permissions() & fs::perms::owner_exec) != fs::perms::none);
#endif
        }
        QCOMPARE(automate::write_launchers(written, "/opt/genko").size(), made.size());  // (its own launchers written again)
        write(made.front(), "#!/bin/sh\necho mine\n");                                   // (a file of the person's own)
        QCOMPARE(thrown([&] { automate::write_launchers(written, "/opt/genko"); }).first, std::string("refused_target"));
        QCOMPARE(storage::read_file(made.front()), std::string("#!/bin/sh\necho mine\n"));
        // its output
        const std::vector<automate::Input> inputs{automate::Input{root / "in" / "a.png", false}};
        QCOMPARE(automate::droplet_output(droplet, inputs, std::nullopt).folder, root / "in" / "droplet-output");
        droplet.output = root / "own";
        QCOMPARE(automate::droplet_output(droplet, inputs, std::nullopt).folder, root / "own");
        QCOMPARE(automate::droplet_output(droplet, inputs, root / "given").folder, root / "given");
        const automate::Output o = automate::droplet_output(droplet, inputs, std::nullopt);
        QCOMPARE(o.format, std::string("jpg"));
        QCOMPARE(o.suffix, std::string("_d"));
        // the runner: 連番 and Also (each page in each folder and format)
        fs::create_directories(root / "in");
        write(root / "in" / "a.png", png(8, 4));
        write(root / "in" / "b.png", png(4, 8));
        automate::Output numbered;
        numbered.folder = root / "n";
        numbered.serial = 1;
        numbered.also.push_back(automate::Output::Also{root / "n2", "tif", std::nullopt});
        const automate::Result result =
            automate::run({automate::Input{root / "in" / "a.png", false}, automate::Input{root / "in" / "b.png", false}}, Json::array(), numbered, automate::Options{});
        QVERIFY2(result.errors.empty(), result.json().dump().c_str());
        QVERIFY(fs::exists(root / "n" / "a_001.png"));
        QVERIFY(fs::exists(root / "n" / "b_002.png"));
        QVERIFY(fs::exists(root / "n2" / "a_001.tif"));
        QCOMPARE(result.files[1]["outputs"], Json::array({core::path_to_utf8(root / "n" / "b_002.png"), core::path_to_utf8(root / "n2" / "b_002.tif")}));
        QCOMPARE(result.files[1]["output"], Json(core::path_to_utf8(root / "n" / "b_002.png")));  // (a picture's "output" kept: P3-6)
#ifndef _WIN32
        // a format that cannot be written after one that was: the file written said with its error (the review's P3-7)
        fs::create_directories(root / "shut");
        fs::permissions(root / "shut", fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
        const bool shut = [&] {
            std::ofstream probe(root / "shut" / "probe");
            return !probe.good();
        }();
        if (shut) {  // (the folder cannot be written in: this user's rights, as the tests run)
            automate::Output half;
            half.folder = root / "half";
            half.also.push_back(automate::Output::Also{root / "shut", "tif", std::nullopt});
            const automate::Result failed = automate::run({automate::Input{root / "in" / "a.png", false}}, Json::array(), half, automate::Options{});
            QCOMPARE(failed.errors.size(), std::size_t(1));
            QCOMPARE(failed.errors[0]["outputs"], Json::array({core::path_to_utf8(root / "half" / "a.png")}));
        }
        fs::permissions(root / "shut", fs::perms::owner_all, fs::perm_options::replace);
        // a link at a launcher's place: never written over
        const fs::path linked = root / "linked.genkodroplet";
        automate::write_droplet(linked, droplet);
        fs::create_symlink(root / "elsewhere.sh", root / "linked.sh");
        QCOMPARE(thrown([&] { automate::write_launchers(linked, "/opt/genko"); }).first, std::string("refused_target"));
        QVERIFY(!fs::exists(root / "elsewhere.sh"));
        // inside a book through a link: refused; a .. that leaves it: not (the review's P3-1)
        fs::create_directory_symlink(root / "b.genko", root / "into");
        QCOMPARE(thrown([&] { automate::write_droplet(root / "into" / "x", droplet); }).first, std::string("refused_target"));
        QVERIFY(thrown([&] { automate::write_droplet(root / "b.genko" / ".." / "out_of_it", droplet); }).first.empty());
        // a steps file that is not a regular file (a FIFO): refused, never read (the review's P3-10)
        const fs::path fifo = root / "steps.fifo";
        if (::mkfifo(fifo.c_str(), 0600) == 0) {
            QCOMPARE(thrown([&] { automate::read_steps_file(fifo); }).first, std::string("value"));
            QCOMPARE(thrown([&] { automate::read_droplet(fifo); }).first, std::string("value"));
        }
#endif
        // a droplet's folder written relative: from the droplet's own folder (the review's P3-4)
        write(root / "rel.genkodroplet", Json{{"genkoDroplet", 1}, {"steps", Json::array()}, {"options", Json{{"output", "out/there"}}}}.dump());
        QCOMPARE(*automate::read_droplet(root / "rel.genkodroplet").output, root / "out" / "there");
    }

    // The steps aimed at the pages: each page of a picture, a book's pages but the covers; an action played on each.
    void stepsAimedAtThePages() {
        const core::Document doc = picture(20, 10);
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

    // The books and folders guarded: an agent's book known (an unnamed caller is legacy:unknown there), a v1–v3 book
    // refused before its lock, an output folder in a book refused before it is made, links to folders never entered (a loop
    // of links ends), the files and folders looked through counted.
    void booksAndFoldersGuarded() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "plain.genko");
        write(root / "plain.genko" / "project.json", "{\"version\": 4}");
        fs::create_directories(root / "gated.genko");
        write(root / "gated.genko" / "project.json", "{\"version\": 4, \"strict_gates\": true}");
        fs::create_directories(root / "drafts.genko" / "studio" / "drafts");
        write(root / "drafts.genko" / "project.json", "{\"version\": 4}");
        QVERIFY(!automate::studio_book(root / "plain.genko"));
        QVERIFY(automate::studio_book(root / "gated.genko"));
        QVERIFY(automate::studio_book(root / "drafts.genko"));
        // a v3 book written in place: refused, its folder as it was (no project.lock)
        fs::create_directories(root / "old.genko");
        write(root / "old.genko" / "project.json", "{\"version\": 3, \"title\": \"x\"}");
        QCOMPARE(thrown([&] { automate::refuse_legacy(root / "old.genko"); }).first, std::string("needs_migration"));
        automate::Output in_place;
        in_place.to = automate::Output::To::Book;
        automate::Options options;
        options.actor = core::Actor("human:leaf");
        automate::Result result = automate::run({automate::Input{root / "old.genko", true}}, Json::array(), in_place, options);
        QCOMPARE(result.errors[0]["code"], Json("needs_migration"));
        std::size_t files = 0;
        for (const auto& e : fs::directory_iterator(root / "old.genko")) files += e.path().filename() == "project.json" ? 0 : 1;
        QCOMPARE(files, std::size_t(0));
        // an output folder in a book (the input's or another): refused before anything is made
        for (const fs::path& inside : {root / "plain.genko" / "out", root / "gated.genko" / "assets" / "x" / "y", root / "plain.genko"}) {
            automate::Output out;
            out.folder = inside;
            QCOMPARE(thrown([&] { automate::run({}, Json::array(), out, options); }).first, std::string("refused_target"));
        }
        QVERIFY(!fs::exists(root / "plain.genko" / "out"));
        QVERIFY(!fs::exists(root / "gated.genko" / "assets"));
        // links to folders: never entered (a loop ends), a picture there taken once
        fs::create_directories(root / "in" / "sub");
        write(root / "in" / "sub" / "a.png", "x");
        std::error_code ec;
        fs::create_directory_symlink(root / "in", root / "in" / "loop1", ec);
        fs::create_directory_symlink(root / "in", root / "in" / "sub" / "loop2", ec);
        automate::InputOptions deep;
        deep.recurse = true;
        if (!ec) {  // (where links cannot be made — Windows without the right —, only this check is left out)
            const automate::Inputs got = automate::list_inputs({root / "in"}, deep);
            QCOMPARE(got.items.size(), std::size_t(1));
            QCOMPARE(got.items[0].path, root / "in" / "sub" / "a.png");
        }
        // the files and folders looked through are counted
        deep.scan_limit = 1;
        QCOMPARE(thrown([&] { automate::list_inputs({root / "in"}, deep); }).first, std::string("value"));
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
        const Json shade = Json::array({Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}}});
        table.bindings.push_back({"everything", "影", std::nullopt, events::action_sha256(shade), std::string("影")});
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
        // an action: run as bound; changed since (in the window or outside), not run
        QVERIFY(!service::actions::store("影", shade));
        const events::Steps played = events::steps_of(loaded.table.bindings[1], events::Event::Print);
        QVERIFY(played.recorded);
        QCOMPARE(played.steps, shade);
        QVERIFY(!service::actions::store("影", Json::array({Json{{"op", "resize_pages"}, {"page", 1}, {"width_px", 9}}})));
        QCOMPARE(thrown([&] { events::steps_of(loaded.table.bindings[1], events::Event::Print); }).first, std::string("script_changed"));
        QVERIFY(!service::actions::remove("影"));
        // an action bound without its sha256, a name too long: not a table
        QCOMPARE(thrown([] { events::from_json(Json{{"bindings", Json::array({Json{{"event", "print"}, {"action", "影"}}})}}); }).first, std::string("value"));
        QCOMPARE(thrown([] {
                     events::from_json(Json{{"bindings", Json::array({Json{{"event", "print"}, {"action", "影"}, {"sha256", "x"}, {"name", std::string(201, 'a')}}})}});
                 }).first,
                 std::string("value"));
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
