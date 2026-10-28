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

#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/open_file.hpp"
#include "service/actions.hpp"
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
        QVERIFY2(out["error"].get<std::string>().rfind("line 2: ", 0) == 0, out.dump().c_str());
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
};

QTEST_GUILESS_MAIN(TestContractAutomate)
#include "test_contract_automate.moc"
