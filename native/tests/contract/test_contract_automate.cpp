// PC5 I-0 (SPEC.md AUTO-01: 自動処理の土台) as a person or a program outside uses it, this build's own contract: `genko
// script` applies a script's steps as `genko apply` applies the same ops (the same reply; the journal names the change by
// the script), refuses the gates, undo and — for an agent — the ops apply_ops does not take, and turns a pseudo op into
// ops; `genko batch` takes a folder's pictures through steps into files コピーを保存 writes byte for byte (again: the next
// free names; --format and --quality; --json), never writes a picture in place, and writes a book's pages (--out) or the
// book itself (--save, the journal naming the script) — an オートアクション of the settings folder too (--action).

#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>

#include "core/json.hpp"
#include "core/command_bus.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "render/ops_registry.hpp"
#include "service/actions.hpp"
#include "service/automate.hpp"
#include "storage/fsutil.hpp"
#include "storage/journal.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

fs::path path_of(const QString& text) { return core::path_from_utf8(text.toStdString()); }
QString q(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream out(file, std::ios::binary);
    out << bytes;
}

// A book of one page of w × h px (a picture's book, written as a .genko).
void write_book(const fs::path& dir, int w, int h) {
    formats::NewImage image;
    image.title = "本";
    image.width = w;
    image.height = h;
    const core::Document doc = formats::new_image(image).doc;
    storage::ProjectLock lock(dir, "human:test");
    lock.try_acquire();
    storage::SaveRequest request;
    request.actor = "human:test";
    request.base_revision = doc.revision;
    request.ops = Json::array();
    storage::Saver(lock).save(doc, request);
}

std::pair<int, Json> cli(const QStringList& args, const QProcessEnvironment& env = {}) {
    const test::Run run = test::run_genko(args, env);
    return {run.exit_code, test::one_line(run.out)};
}

std::string last_script(const fs::path& book) {
    const storage::journal::Stacks stacks = storage::journal::stacks(book);
    return stacks.undo.empty() ? std::string("(none)") : stacks.undo.back().script;
}

}  // namespace

class TestContractAutomate : public QObject {
    Q_OBJECT

private slots:
    void scriptAppliesAsApply() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        write_book(root / "a.genko", 40, 20);
        fs::copy(root / "a.genko", root / "b.genko", fs::copy_options::recursive);  // (the same book twice: its ids too)
        fs::remove(root / "b.genko" / "project.lock");
        const Json ops = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "メモ"}},
                                      Json{{"op", "add_layer"}, {"page", 1}, {"kind", "paint"}, {"id", "L1"}, {"name", "影"}}});
        write(root / "ops.json", ops.dump());
        write(root / "影を足す.txt", "# 影を足す\nset_note {\"page\": 1, \"note\": \"メモ\"}\n\nadd_layer {\"page\": 1, \"kind\": \"paint\", \"id\": \"L1\", \"name\": \"影\"}\n");
        const auto [code_a, applied] = cli({QStringLiteral("apply"), q(root / "a.genko"), q(root / "ops.json")});
        const auto [code_b, scripted] = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "影を足す.txt")});
        QCOMPARE(code_a, 0);
        QCOMPARE(code_b, 0);
        for (const char* key : {"ok", "applied", "snapshot", "warnings", "revision"}) QCOMPARE(scripted[key], applied[key]);
        QCOMPARE(scripted.size(), applied.size());
        QVERIFY(scripted.contains("txn") && scripted.contains("job_id"));
        QCOMPARE(last_script(root / "a.genko"), std::string());
        QCOMPARE(last_script(root / "b.genko"), std::string("影を足す"));
        // --name, a JSON form, --dry-run (nothing written)
        write(root / "s.json", Json{{"steps", Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}})}}.dump());
        auto [code, out] = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "s.json"), QStringLiteral("--dry-run")});
        QCOMPARE(code, 0);
        QCOMPARE(out["revision"], scripted["revision"]);
        std::tie(code, out) = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "s.json"), QStringLiteral("--name"), QStringLiteral("メモを直す")});
        QCOMPARE(code, 0);
        QCOMPARE(last_script(root / "b.genko"), std::string("メモを直す"));
        QCOMPARE(storage::load_document(root / "b.genko").document.page(0).note, std::string("x"));
    }

    void scriptRefusesWhatNoStepMayBe() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        write_book(root / "b.genko", 40, 20);
        const std::int64_t revision = storage::load_document(root / "b.genko").document.revision;
        const std::vector<std::pair<std::string, QStringList>> refused{
            {"set_note {\"page\": 1, \"note\": \"a\"}\nname_ok {\"page\": 1}\n", {}},
            {"[{\"op\": \"approve\", \"gate\": \"name\", \"page\": 1}]", {}},
            {"[{\"op\": \"for_pages\", \"pages\": \"all\", \"ops\": [{\"op\": \"lock_page\", \"agent\": \"x\"}]}]", {}},
            {"undo\n", {}},
            {"resize_pages {\"page\": 1, \"width_px\": 20}\n", {QStringLiteral("--agent"), QStringLiteral("ai:hermes")}},
            {"fit_image {\"page\": 1, \"width_px\": 20, \"height_px\": 20}\n", {QStringLiteral("--agent"), QStringLiteral("ai:hermes")}},
        };
        for (const auto& [steps, extra] : refused) {
            write(root / "s.txt", steps);
            const auto [code, out] = cli(QStringList{QStringLiteral("script"), q(root / "b.genko"), q(root / "s.txt")} + extra);
            QVERIFY2(code == 1, steps.c_str());
            QCOMPARE(out["ok"], Json(false));
            QVERIFY2(out["code"] == Json("steps"), out.dump().c_str());
            QCOMPARE(storage::load_document(root / "b.genko").document.revision, revision);
        }
        // an agent's own ops go through
        write(root / "s.txt", "set_note {\"page\": 1, \"note\": \"ai\"}\n");
        auto [code, out] = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "s.txt"), QStringLiteral("--agent"), QStringLiteral("ai:hermes")});
        QCOMPARE(code, 0);
        // a pseudo op: made ops on the book as it is (40 × 20 into 20 × 20: 20 × 10)
        write(root / "fit.txt", "fit_image {\"page\": 1, \"width_px\": 20, \"height_px\": 20}\n");
        std::tie(code, out) = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "fit.txt")});
        QCOMPARE(code, 0);
        QCOMPARE(out["applied"], Json::array({"resize_pages"}));
        const core::PageSpec spec = storage::load_document(root / "b.genko").document.page(0).spec;
        QCOMPARE(std::round(spec.width_mm.value() / 25.4 * spec.dpi.value()), 20.0);
        QCOMPARE(std::round(spec.height_mm.value() / 25.4 * spec.dpi.value()), 10.0);
        // a line that cannot be read: its number
        write(root / "bad.txt", "set_note {\"page\": 1}\nset_note {\n");
        std::tie(code, out) = cli({QStringLiteral("script"), q(root / "b.genko"), q(root / "bad.txt")});
        QCOMPARE(code, 1);
        QVERIFY2(out["error"].get<std::string>().rfind("steps, line 2: ", 0) == 0, out.dump().c_str());
    }

    void batchOfPictures() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "in");
        for (const auto& [name, w] : {std::pair<const char*, int>{"a.png", 30}, {"b.png", 12}}) {
            formats::NewImage image;
            image.title = name;
            image.width = w;
            image.height = 10;
            image.background = "#336699";
            formats::write_copy(formats::new_image(image).doc, 0, root / "in" / name);
        }
        write(root / "in" / ".hidden.png", "x");
        write(root / "in" / "readme.txt", "x");
        write(root / "none.json", "[]");
        auto [code, out] = cli({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--out"), q(root / "out"), QStringLiteral("--steps"),
                                q(root / "none.json"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(out["ok"], Json(true));
        QCOMPARE(out["files"].size(), std::size_t(2));
        QCOMPARE(out["errors"], Json::array());
        // what コピーを保存 writes of each picture opened, byte for byte
        for (const char* name : {"a.png", "b.png"}) {
            const formats::Opened opened = formats::open_file(storage::read_file(root / "in" / name), name);
            formats::write_copy(opened.doc, 0, root / (std::string("copy_") + name));
            QCOMPARE(storage::read_file(root / "out" / name), storage::read_file(root / (std::string("copy_") + name)));
        }
        // again: the next free names
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "out"), QStringLiteral("--steps"),
                                   q(root / "none.json"), QStringLiteral("--json")});
        QCOMPARE(out["files"][0]["output"], Json(core::path_to_utf8(root / "out" / "a-2.png")));
        // --format and --quality, the lines of the files written without --json
        const test::Run plain = test::run_genko({QStringLiteral("batch"), q(root / "in" / "b.png"), QStringLiteral("--out"), q(root / "jpg"), QStringLiteral("--steps"),
                                                 q(root / "none.json"), QStringLiteral("--format"), QStringLiteral("jpg"), QStringLiteral("--quality"), QStringLiteral("8"),
                                                 QStringLiteral("--suffix"), QStringLiteral("_web")});
        QCOMPARE(plain.exit_code, 0);
        QCOMPARE(plain.out.toStdString(), core::path_to_utf8(root / "jpg" / "b_web.jpg") + "\n");
        // steps on each picture (fit_image without a page: each page of it)
        write(root / "fit.txt", "fit_image {\"width_px\": 6, \"height_px\": 6}\n");
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--out"), q(root / "small"), QStringLiteral("--steps"),
                                   q(root / "fit.txt"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        const formats::Opened small = formats::open_file(storage::read_file(root / "small" / "a.png"), "a.png");
        const core::PageSpec& spec = small.doc.page(0).spec;
        QCOMPARE(std::round(spec.width_mm.value() / 25.4 * spec.dpi.value()), 6.0);
        QCOMPARE(std::round(spec.height_mm.value() / 25.4 * spec.dpi.value()), 2.0);
        // a picture is never written in place; a gate never a step; the arguments
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "in" / "a.png"), QStringLiteral("--save"), QStringLiteral("--steps"), q(root / "none.json"),
                                   QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QCOMPARE(out["errors"][0]["code"], Json("refused_target"));
        write(root / "gate.txt", "lock_page {\"page\": 1, \"agent\": \"x\"}\n");
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--out"), q(root / "x"), QStringLiteral("--steps"), q(root / "gate.txt")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("steps"));
        QVERIFY(!fs::exists(root / "x" / "a.png"));
        QCOMPARE(test::run_genko({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--steps"), q(root / "none.json")}).exit_code, 2);
        QCOMPARE(test::run_genko({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--out"), q(root / "x")}).exit_code, 2);
        QCOMPARE(test::run_genko({QStringLiteral("batch"), q(root / "in"), QStringLiteral("--out"), q(root / "x"), QStringLiteral("--steps"), q(root / "none.json"),
                                  QStringLiteral("--quality"), QStringLiteral("13")})
                     .exit_code,
                 2);
    }

    // An agent's book (strict_gates, its name approved): an unnamed `genko batch --save` is legacy:unknown there, refused
    // as `genko apply` refuses the same steps; a person named goes through.
    void batchInAnAgentsBookAsApply() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        core::Document doc = core::new_episode("t", core::Num(1), 1, core::PageSpec::b4_comic());
        doc.strict_gates = true;
        doc.pages[0]->name_ok = true;
        const std::string frame = doc.pages[0]->leaf_frames()[0]->id;
        {
            storage::ProjectLock lock(root / "s.genko", "human:test");
            lock.try_acquire();
            storage::SaveRequest request;
            request.actor = "human:test";
            request.ops = Json::array();
            storage::Saver(lock).save(doc, request);
        }
        const Json split{{"op", "split_frame"}, {"frame_id", frame}, {"axis", "horizontal"}, {"ratio", 0.5}, {"gutter_mm", 2}};
        Json on_page = split;
        on_page["page"] = 1;
        write(root / "ops.json", Json::array({on_page}).dump());
        write(root / "split.json", Json::array({split}).dump());
        const std::int64_t revision = storage::load_document(root / "s.genko").document.revision;
        auto [code, out] = cli({QStringLiteral("apply"), q(root / "s.genko"), q(root / "ops.json")});
        QCOMPARE(code, 1);
        QVERIFY2(out["error"].get<std::string>().find("the name is approved") != std::string::npos, out.dump().c_str());
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "s.genko"), QStringLiteral("--save"), QStringLiteral("--steps"), q(root / "split.json"),
                                   QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QVERIFY2(out["errors"][0]["error"].get<std::string>().find("the name is approved") != std::string::npos, out.dump().c_str());
        QCOMPARE(storage::load_document(root / "s.genko").document.revision, revision);
        // a person named: as `genko apply --agent human:…`
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "s.genko"), QStringLiteral("--save"), QStringLiteral("--steps"), q(root / "split.json"),
                                   QStringLiteral("--agent"), QStringLiteral("human:leaf"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(storage::load_document(root / "s.genko").document.revision, revision + 1);
        // an agent's ops only for legacy:unknown (resize_pages is not one)
        write(root / "resize.txt", "resize_pages {\"width_px\": 100}\n");
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "s.genko"), QStringLiteral("--save"), QStringLiteral("--steps"), q(root / "resize.txt"),
                                   QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QCOMPARE(out["errors"][0]["code"], Json("steps"));
        // the output folder inside the book: refused, not made
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "s.genko"), QStringLiteral("--out"), q(root / "s.genko" / "out"), QStringLiteral("--steps"),
                                   q(root / "resize.txt")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("refused_target"));
        QVERIFY(!fs::exists(root / "s.genko" / "out"));
    }

    void batchOfBooks() {
        QTemporaryDir dir;
        QTemporaryDir config;
        const fs::path root = path_of(dir.path());
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config.path());
        write_book(root / "作品.genko", 40, 20);
        // --save: the steps on each page, the book written, the journal naming the script
        write(root / "メモ.txt", "set_note {\"note\": \"batch\"}\n");
        auto [code, out] = cli({QStringLiteral("batch"), q(root / "作品.genko"), QStringLiteral("--save"), QStringLiteral("--steps"), q(root / "メモ.txt"),
                                QStringLiteral("--json")},
                               env);
        QCOMPARE(code, 0);
        QCOMPARE(out["files"][0]["revision"], Json(2));
        QCOMPARE(storage::load_document(root / "作品.genko").document.page(0).note, std::string("batch"));
        QCOMPARE(last_script(root / "作品.genko"), std::string("メモ"));
        // --action: an オートアクション of the settings folder, played on each page; --out: each page a file
        qputenv("GENKO_CONFIG_DIR", config.path().toUtf8());
        QVERIFY(!service::actions::store("ノート", Json::array({Json{{"op", "set_note"}, {"page", 5}, {"note", "action"}}})));
        qunsetenv("GENKO_CONFIG_DIR");
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "作品.genko"), QStringLiteral("--out"), q(root / "out"), QStringLiteral("--action"),
                                   QStringLiteral("ノート"), QStringLiteral("--json")},
                                  env);
        QCOMPARE(code, 0);
        QCOMPARE(out["files"][0]["outputs"], Json::array({core::path_to_utf8(root / "out" / core::path_from_utf8("作品_p001.png"))}));
        QCOMPARE(storage::load_document(root / "作品.genko").document.page(0).note, std::string("batch"));  // (the book itself unchanged)
        std::tie(code, out) = cli({QStringLiteral("batch"), q(root / "作品.genko"), QStringLiteral("--dry-run"), QStringLiteral("--action"), QStringLiteral("無い")},
                                  env);
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("not_found"));
    }

    // PC5 I-e: `genko automate` — 画像プロセッサー (a folder of an 8-bit PNG, a 16-bit TIFF and a CMYK JPEG written in three
    // formats, each into its folder; fitted, never enlarged; sRGB), 画像を合わせる (拡大しない), 条件付きモード変更 (only the
    // modes listed), レンズ補正 (the same pixels as the filter on each picture one by one; a picture without the EXIF's focal
    // length said and the next one taken) — and an agent refused what its ops cannot do.
    void automateCommands() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "in");
        for (const auto& [name, mode, depth] : {std::tuple<const char*, const char*, int>{"a.png", "rgb", 8}, {"b.tif", "rgb", 16}, {"c.jpg", "cmyk", 8}}) {
            formats::NewImage image;
            image.title = name;
            image.width = 60;
            image.height = 30;
            image.mode = mode;
            image.depth = depth;
            image.background = "#336699";
            formats::write_copy(formats::new_image(image).doc, 0, root / "in" / name);
        }
        const auto opened = [](const fs::path& file) { return formats::open_file(storage::read_file(file), core::path_to_utf8(file.filename())).doc; };
        const auto px = [](const core::Document& doc) {
            const core::PageSpec& s = doc.page(0).spec;
            return std::pair<double, double>{std::round(s.width_mm.value() / 25.4 * s.dpi.value()), std::round(s.height_mm.value() / 25.4 * s.dpi.value())};
        };
        QCOMPARE(opened(root / "in" / "c.jpg").page(0).color.has_value(), true);  // (a CMYK picture: a page of inks)
        auto [code, out] = cli({QStringLiteral("automate"), QStringLiteral("image-processor"), q(root / "in"), QStringLiteral("--out"), q(root / "out"),
                                QStringLiteral("--formats"), QStringLiteral("jpg,psd,tif"), QStringLiteral("--width"), QStringLiteral("20"),
                                QStringLiteral("--height"), QStringLiteral("20"), QStringLiteral("--srgb"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(out["files"].size(), std::size_t(3));
        for (const char* stem : {"a", "b", "c"}) {
            for (const auto& [folder, ext] : {std::pair<const char*, const char*>{"JPEG", "jpg"}, {"PSD", "psd"}, {"TIFF", "tif"}}) {
                const fs::path file = root / "out" / folder / (std::string(stem) + "." + ext);
                QVERIFY2(fs::exists(file), core::path_to_utf8(file).c_str());
                const core::Document doc = opened(file);
                QCOMPARE(px(doc), (std::pair<double, double>{20.0, 10.0}));
                QVERIFY2(!doc.page(0).color, core::path_to_utf8(file).c_str());  // (sRGB: an RGB page, no inks)
            }
        }
        // never enlarged
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("image-processor"), q(root / "in" / "a.png"), QStringLiteral("--out"),
                                   q(root / "big"), QStringLiteral("--formats"), QStringLiteral("png"), QStringLiteral("--width"), QStringLiteral("600"),
                                   QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(px(opened(root / "big" / "PNG" / "a.png")), (std::pair<double, double>{60.0, 30.0}));
        // 画像を合わせる: 15 × 8 in a box of 15; --dont-enlarge leaves a smaller picture as it is
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("fit"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "fit"),
                                   QStringLiteral("--width"), QStringLiteral("15"), QStringLiteral("--height"), QStringLiteral("15"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(px(opened(root / "fit" / "a.png")), (std::pair<double, double>{15.0, 8.0}));
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("fit"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "fit"),
                                   QStringLiteral("--width"), QStringLiteral("600"), QStringLiteral("--height"), QStringLiteral("600"),
                                   QStringLiteral("--dont-enlarge"), QStringLiteral("--suffix"), QStringLiteral("_same"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(px(opened(root / "fit" / "a_same.png")), (std::pair<double, double>{60.0, 30.0}));
        // 条件付きモード変更: the CMYK picture made grey, the RGB ones left
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("mode"), q(root / "in"), QStringLiteral("--out"), q(root / "mode"),
                                   QStringLiteral("--from"), QStringLiteral("cmyk,lab"), QStringLiteral("--to"), QStringLiteral("gray"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        QCOMPARE(service::automate::page_mode(opened(root / "mode" / "c.jpg").page(0)), std::string("gray"));
        QCOMPARE(service::automate::page_mode(opened(root / "mode" / "a.png").page(0)), std::string("rgb"));
        QCOMPARE(storage::read_file(root / "mode" / "a.png"), [&] {  // (as コピーを保存 writes the picture untouched)
            formats::write_copy(opened(root / "in" / "a.png"), 0, root / "a_copy.png");
            return storage::read_file(root / "a_copy.png");
        }());
        // レンズ補正: the filter of the batch, the filter on the picture by itself — the same file
        const Json settings{{"profile", "generic"}, {"focal_length", 24}, {"edge", "black"}};
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("lens"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "lens"),
                                   QStringLiteral("--settings"), QString::fromStdString(settings.dump()), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        {
            const core::Document doc = opened(root / "in" / "a.png");
            Json op{{"op", "filter_raster"}, {"page", 1}, {"id", doc.page(0).layers[0].id}, {"kind", "lens_correction"}};
            op.update(settings);
            const core::Document by_hand = core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test")).doc;
            formats::write_copy(by_hand, 0, root / "lens_by_hand.png");
            QCOMPARE(storage::read_file(root / "lens" / "a.png"), storage::read_file(root / "lens_by_hand.png"));
            QVERIFY(storage::read_file(root / "lens" / "a.png") != storage::read_file(root / "a_copy.png"));
        }
        // the profile auto: no EXIF's focal length — each picture said, none stops the others
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("lens"), q(root / "in"), QStringLiteral("--out"), q(root / "auto"),
                                   QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QCOMPARE(out["errors"].size(), std::size_t(3));
        QVERIFY2(out["errors"][2]["error"].get<std::string>().find("focal length") != std::string::npos, out.dump().c_str());
        // an agent: 画像を合わせる stands for resize_pages, not one of its ops
        std::tie(code, out) = cli({QStringLiteral("automate"), QStringLiteral("fit"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "ai"),
                                   QStringLiteral("--width"), QStringLiteral("10"), QStringLiteral("--agent"), QStringLiteral("ai:hermes"), QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QCOMPARE(out["errors"][0]["code"], Json("steps"));
        QVERIFY(!fs::exists(root / "ai" / "a.png"));
        // the arguments: each kind its own options
        for (const QStringList& wrong : {QStringList{QStringLiteral("automate"), QStringLiteral("nope"), q(root / "in")},
                                         QStringList{QStringLiteral("automate"), QStringLiteral("fit"), q(root / "in"), QStringLiteral("--out"), q(root / "x")},
                                         QStringList{QStringLiteral("automate"), QStringLiteral("fit"), q(root / "in"), QStringLiteral("--out"), q(root / "x"),
                                                     QStringLiteral("--width"), QStringLiteral("5"), QStringLiteral("--to"), QStringLiteral("gray")},
                                         QStringList{QStringLiteral("automate"), QStringLiteral("image-processor"), q(root / "in"), QStringLiteral("--save")},
                                         QStringList{QStringLiteral("automate"), QStringLiteral("mode"), q(root / "in"), QStringLiteral("--dry-run")},
                                         QStringList{QStringLiteral("automate"), QStringLiteral("lens"), q(root / "b.genko"), QStringLiteral("--save")}}) {
            QVERIFY2(test::run_genko(wrong).exit_code == 2, wrong.join(QLatin1Char(' ')).toUtf8().constData());
        }
        QVERIFY(!fs::exists(root / "x"));
        // the pseudo ops stay the steps' own: `genko apply` does not know them
        write_book(root / "b.genko", 40, 20);
        write(root / "fit.json", Json::array({Json{{"op", "fit_image"}, {"page", 1}, {"width_px", 10}}}).dump());
        std::tie(code, out) = cli({QStringLiteral("apply"), q(root / "b.genko"), q(root / "fit.json")});
        QCOMPARE(code, 1);
    }

    // PC5 I-e: ドロップレット — made by `genko droplet --create` (the file, its launchers beside it: one fixed line, runnable),
    // run by `genko droplet` and by its launcher (the files given, written into droplet-output beside them); a gate never in
    // one; an agent running it refused what its ops cannot do. Everything made inside the test's folder (removed after it).
    void droplets() {
        QTemporaryDir dir;
        const fs::path root = path_of(dir.path());
        fs::create_directories(root / "in");
        fs::create_directories(root / "d");
        for (const char* name : {"a.png", "b.png"}) {
            formats::NewImage image;
            image.title = name;
            image.width = 40;
            image.height = 20;
            image.background = "#993366";
            formats::write_copy(formats::new_image(image).doc, 0, root / "in" / name);
        }
        write(root / "fit.txt", "# 小さく\nfit_image {\"width_px\": 10, \"height_px\": 10}\n");
        auto [code, out] = cli({QStringLiteral("droplet"), QStringLiteral("--create"), q(root / "d" / "小さく"), QStringLiteral("--steps"), q(root / "fit.txt"),
                                QStringLiteral("--suffix"), QStringLiteral("_s"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        const fs::path droplet = root / "d" / core::path_from_utf8("小さく.genkodroplet");
        QCOMPARE(out["droplet"], Json(core::path_to_utf8(droplet)));
        QVERIFY(fs::exists(droplet));
        const Json kept = core::parse_python_json(storage::read_file(droplet));
        QCOMPARE(kept["genkoDroplet"], Json(1));
        QCOMPARE(kept["name"], Json("小さく"));
        QCOMPARE(kept["steps"], Json::array({Json{{"op", "fit_image"}, {"width_px", 10}, {"height_px", 10}}}));
        QCOMPARE(out["launchers"].size(), service::automate::launchers_here().size());
#ifndef _WIN32
        const fs::path shell = root / "d" / core::path_from_utf8("小さく.sh");
        QCOMPARE(out["launchers"][0], Json(core::path_to_utf8(shell)));
        QVERIFY((fs::status(shell).permissions() & fs::perms::owner_exec) != fs::perms::none);
        const std::string text = storage::read_file(shell);
        QVERIFY2(text.find(" droplet '" + core::path_to_utf8(droplet) + "' \"$@\"\n") != std::string::npos, text.c_str());
        QVERIFY(text.find("--agent") == std::string::npos);
        QVERIFY(fs::exists(root / "d" / core::path_from_utf8("小さく.desktop")));
#endif
        // run: droplet-output beside the files
        std::tie(code, out) = cli({QStringLiteral("droplet"), q(droplet), q(root / "in" / "a.png"), QStringLiteral("--json")});
        QVERIFY2(code == 0, out.dump().c_str());
        const fs::path made = root / "in" / "droplet-output" / "a_s.png";
        QCOMPARE(out["files"][0]["output"], Json(core::path_to_utf8(made)));
        const core::PageSpec spec = formats::open_file(storage::read_file(made), "a_s.png").doc.page(0).spec;
        QCOMPARE(std::round(spec.width_mm.value() / 25.4 * spec.dpi.value()), 10.0);
        QCOMPARE(std::round(spec.height_mm.value() / 25.4 * spec.dpi.value()), 5.0);
#ifndef _WIN32
        // its launcher, as a file manager runs it with the files dropped
        QProcess launcher;
        launcher.start(QStringLiteral("/bin/sh"), {q(shell), q(root / "in" / "b.png")});
        QVERIFY(launcher.waitForFinished(120000));
        QCOMPARE(launcher.exitCode(), 0);
        QVERIFY(fs::exists(root / "in" / "droplet-output" / "b_s.png"));
#endif
        // --out given; an agent: refused per file, nothing written
        std::tie(code, out) = cli({QStringLiteral("droplet"), q(droplet), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "o"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QVERIFY(fs::exists(root / "o" / "a_s.png"));
        std::tie(code, out) = cli({QStringLiteral("droplet"), q(droplet), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "ai"),
                                   QStringLiteral("--agent"), QStringLiteral("ai:hermes"), QStringLiteral("--json")});
        QCOMPARE(code, 1);
        QCOMPARE(out["errors"][0]["code"], Json("steps"));
        QVERIFY(!fs::exists(root / "ai" / "a_s.png"));
        // a gate is never a droplet's step: nothing made
        write(root / "gate.txt", "approve {\"gate\": \"name\", \"page\": 1}\n");
        std::tie(code, out) = cli({QStringLiteral("droplet"), QStringLiteral("--create"), q(root / "d" / "gate"), QStringLiteral("--steps"), q(root / "gate.txt")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("steps"));
        QVERIFY(!fs::exists(root / "d" / "gate.genkodroplet"));
        QVERIFY(!fs::exists(root / "d" / "gate.sh"));
        // a droplet with a gate written by hand: refused when it runs
        write(root / "d" / "hand.genkodroplet", Json{{"genkoDroplet", 1}, {"name", "h"}, {"steps", Json::array({Json{{"op", "lock_page"}, {"agent", "x"}}})}}.dump());
        std::tie(code, out) = cli({QStringLiteral("droplet"), q(root / "d" / "hand.genkodroplet"), q(root / "in" / "a.png"), QStringLiteral("--out"), q(root / "h")});
        QCOMPARE(code, 1);
        QCOMPARE(out["code"], Json("steps"));
        // --no-launcher; the arguments
        std::tie(code, out) = cli({QStringLiteral("droplet"), QStringLiteral("--create"), q(root / "d" / "only"), QStringLiteral("--steps"), q(root / "fit.txt"),
                                   QStringLiteral("--no-launcher"), QStringLiteral("--json")});
        QCOMPARE(code, 0);
        QCOMPARE(out["launchers"], Json::array());
        QVERIFY(!fs::exists(root / "d" / "only.sh") && !fs::exists(root / "d" / "only.lnk"));
        QCOMPARE(test::run_genko({QStringLiteral("droplet"), q(droplet)}).exit_code, 2);
        QCOMPARE(test::run_genko({QStringLiteral("droplet"), q(droplet), q(root / "in"), QStringLiteral("--steps"), q(root / "fit.txt")}).exit_code, 2);
        QCOMPARE(test::run_genko({QStringLiteral("droplet"), QStringLiteral("--create"), q(root / "d" / "x")}).exit_code, 2);
    }
};

QTEST_GUILESS_MAIN(TestContractAutomate)
#include "test_contract_automate.moc"
