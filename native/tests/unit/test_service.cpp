// The studio service where this build goes past Python (SPEC COMP-01a, M5②) and what its contract test cannot show:
// background jobs (a job's record from running to done or failed, waited for before the process ends; a job lost with
// its process; ids that are not a job's; a job whose thread cannot start), a palette picture (a GIF) kept as a palette
// PNG, the official export of formats::run handed to the studio (HumanService.export: its preflight) after Python's
// own refusals, holding the book while it writes and taking back what it wrote when its approval cannot be saved, with
// the dots (screen) a person asked for; adopt-drafts refused before the book is saved when its folder cannot be renamed;
// the ops an agent may send kept inside for_pages too, and the files put_raster reads; preview names made of ids kept
// in the book; a page gone between a tool's two reads; books of v1–v3 refused before anything is written; several
// character sheets asked for in one save; chat approvals checked again under the lock; the lock another process
// holds; the keyword confine kept from callers; the command line's own refusals.

#include <QtTest>

#include <QDir>
#include <QProcess>
#include <QTemporaryDir>

#include <chrono>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

#include "core/base64.hpp"
#include "core/clock.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "formats/exporting.hpp"
#include "formats/pillow_save.hpp"
#include "render/image.hpp"
#include "render/page.hpp"
#include "render/placed.hpp"
#include "service/book.hpp"
#include "service/evaluate.hpp"
#include "service/human_service.hpp"
#include "service/jobs.hpp"
#include "service/studio_service.hpp"
#include "service/upscale.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace fs = std::filesystem;
namespace jobs = genko::service::jobs;

namespace {

double now_seconds() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// The smallest GIF with a coloured palette (red and blue): Pillow opens it as a "P" picture.
std::string palette_gif() {
    const unsigned char bytes[] = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x01, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
                                   0xff, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x02, 0x02, 0x44, 0x01, 0x00, 0x3b};
    return std::string(reinterpret_cast<const char*>(bytes), sizeof bytes);
}

std::size_t files_under(const fs::path& folder) {
    std::size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) n += it->is_regular_file() ? 1 : 0;
    return n;
}

QString q(const fs::path& path) { return QString::fromStdString(genko::core::path_to_utf8(path)); }

// A grey picture as a PNG.
std::string grey_png(int width, int height, int grey) {
    return genko::formats::png_bytes(genko::render::Image::create("RGB", genko::render::Size{width, height}, genko::render::Ink{grey, grey, grey}));
}

// A hand-drawn name's scan: white paper with three panel borders in black (an A5 page's shape).
std::string scan_png() {
    genko::render::Image image = genko::render::Image::create("L", genko::render::Size{296, 420}, genko::render::Ink(255));
    for (const auto& [x0, y0, x1, y1] : std::vector<std::array<int, 4>>{{12, 12, 284, 150}, {12, 162, 140, 408}, {152, 162, 284, 408}}) {
        image.paste(genko::render::Ink(0), genko::render::Box{x0, y0, x1 + 1, y0 + 3});
        image.paste(genko::render::Ink(0), genko::render::Box{x0, y1 - 2, x1 + 1, y1 + 1});
        image.paste(genko::render::Ink(0), genko::render::Box{x0, y0, x0 + 3, y1 + 1});
        image.paste(genko::render::Ink(0), genko::render::Box{x1 - 2, y0, x1 + 1, y1 + 1});
    }
    return genko::formats::png_bytes(image);
}

// The revision project.json says.
std::int64_t revision_of(const fs::path& book) { return genko::storage::read_disk_state(book).revision; }

// The code of the core::Error `work` throws ("" when it throws none; "<what>" for another exception).
std::string code_thrown(const std::function<void()>& work) {
    try {
        work();
    } catch (const genko::core::Error& error) {
        return error.code();
    } catch (const std::exception& error) {
        return std::string("<") + error.what() + ">";
    }
    return {};
}

// A book of `pages` pages (A5) written as a person's first save; `stage` each page's stage.
fs::path plain_book(const fs::path& dir, int pages, const std::string& stage = "name") {
    genko::core::Document doc = genko::core::new_episode("plain", genko::core::Num(1), pages, genko::core::PageSpec::a5_doujin());
    for (int i = 0; i < pages; ++i) doc.edit_page(static_cast<std::size_t>(i)).stage = stage;
    fs::create_directories(dir);
    genko::storage::ProjectLock lock(dir, "human:leaf");
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = "human:leaf";
    genko::storage::Saver(lock).save(doc, request);
    return dir;
}

// Ops applied and saved as a person.
void as_person(const fs::path& book, const Json& ops) {
    genko::service::Session session(book, "human:leaf");
    session.apply(ops, "human:leaf");
    session.save("human:leaf");
}

// While it lives, new_id() gives ids no book has (counted from far away) and, once, at the first id taken when `when`
// says so, runs `act` — what another writer does between a tool's steps, at a point the tool reaches by reading the
// book (each read takes ids). `act` takes ids too (given the same way, without acting again).
class IdHook {
public:
    IdHook(std::function<bool()> when, std::function<void()> act)
        : state_(std::make_shared<State>(std::move(when), std::move(act))), source_([state = state_] { return state->next(); }) {}

    bool fired() const { return state_->fired; }
    // What `act` threw (rethrown here), if anything.
    void check() const {
        if (state_->error) std::rethrow_exception(state_->error);
    }

private:
    struct State {
        State(std::function<bool()> w, std::function<void()> a) : when(std::move(w)), act(std::move(a)) {}
        std::function<bool()> when;
        std::function<void()> act;
        genko::core::ScopedIdSource::Source count = genko::core::counting_ids(700'000'000'000ULL);
        bool fired = false;
        bool acting = false;
        std::exception_ptr error;

        std::string next() {
            if (!fired && !acting && when()) {
                fired = true;
                acting = true;
                try {
                    act();
                } catch (...) {
                    error = std::current_exception();
                }
                acting = false;
            }
            return count();
        }
    };
    std::shared_ptr<State> state_;
    genko::core::ScopedIdSource source_;
};

// The writing tools, each with arguments it takes (a book of v1–v3 refuses them all before it reads them).
std::vector<std::pair<std::string, Json>> writing_calls(const std::string& png) {
    return {
        {"import_image", Json{{"png_base64", png}}},
        {"apply_ops", Json{{"ops", Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}})}}},
        {"apply_ops", Json{{"ops", Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}})}, {"commit", true}}},
        {"set_bible", Json{{"bible", Json{{"characters", Json::array()}}}}},
        {"set_script", Json{{"script", Json{{"scenes", Json::array()}}}}},
        {"submit_name", Json{{"plan", Json{{"page", 1}, {"layout", "4koma"}, {"panels", Json::array()}}}}},
        {"record_review", Json{{"page", 1}, {"score", 0.5}, {"notes", "x"}}},
        {"request_approval", Json{{"gate", "name"}, {"pages", Json::array({1})}}},
        {"record_chat_approval", Json{{"gate", "name"}, {"message", "OK"}, {"pages", Json::array({1})}}},
        {"generation_request", Json{{"page", 1}, {"frame_id", "f1"}}},
        {"import_name", Json{{"files", Json::array({"x.png"})}}},
        {"ask_human", Json{{"text", "?"}}},
        {"undo", Json::object()},
        {"finish_page", Json{{"page", 1}}},
    };
}

}  // namespace

class TestService : public QObject {
    Q_OBJECT

    QTemporaryDir dir_;

    fs::path folder(const QString& name) const {
        const fs::path out = genko::core::path_from_utf8((dir_.path() + QLatin1Char('/') + name).toStdString());
        fs::create_directories(out);
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(dir_.isValid()); }

    // A job that is not done within the wait is "running" (its id for export_status); when its work ends, the record
    // says done with the reply. wait_all() returns only when it has ended.
    void jobsRecordTheirWork() {
        const fs::path project = folder(QStringLiteral("jobs-a"));
        std::promise<void> go;
        std::shared_future<void> started = go.get_future().share();
        const Json reply = jobs::start(
            project, "export",
            [started] {
                started.wait();
                return Json{{"ok", true}, {"files", Json::array({"x.png"})}};
            },
            "ai:test", 0.05);
        QCOMPARE(reply["ok"], Json(true));
        QCOMPARE(reply["status"], Json("running"));
        QCOMPARE(reply["kind"], Json("export"));
        const std::string id = reply["job"].get<std::string>();
        QVERIFY(id.starts_with("job_"));
        Json status = jobs::status(project, Json(id));
        QCOMPARE(status["status"], Json("running"));
        QVERIFY(status["seconds"].is_number_float());
        QVERIFY(!status.contains("result"));
        go.set_value();
        jobs::wait_all();
        status = jobs::status(project, Json(id));
        QCOMPARE(status["status"], Json("done"));
        QCOMPARE(status["result"], (Json{{"ok", true}, {"files", Json::array({"x.png"})}}));
        const Json record = genko::core::parse_python_json(genko::storage::read_file(project / "studio" / "jobs" / (id + ".json")));
        QCOMPARE(record["actor"], Json("ai:test"));
        QVERIFY(record["finished"].get<double>() >= record["started"].get<double>());
    }

    // What the work threw is the job's error, as Python's f"{type(exc).__name__}: {exc}" says it; a reply that is not
    // ok is a failed job.
    void jobsSayWhatBroke() {
        const fs::path project = folder(QStringLiteral("jobs-b"));
        const Json thrown = jobs::start(project, "upscale", []() -> Json { throw genko::core::PyValueError("scale は 1〜4"); }, "ai:test", 30);
        QCOMPARE(thrown["ok"], Json(false));
        QCOMPARE(thrown["error"], Json("ValueError: scale は 1〜4"));
        const Json failed = jobs::start(project, "export", [] { return Json{{"ok", false}, {"error", "no"}}; }, "ai:test", 30);
        jobs::wait_all();
        QCOMPARE(jobs::status(project, thrown["job"])["status"], Json("failed"));
        QCOMPARE(jobs::status(project, failed["job"])["status"], Json("failed"));
        QCOMPARE(jobs::status(project, failed["job"])["result"]["error"], Json("no"));
        QCOMPARE(jobs::exception_text(std::make_exception_ptr(genko::core::PyUncaught("KeyError", "'x'"))), std::string("KeyError: 'x'"));
        QCOMPARE(jobs::exception_text(std::make_exception_ptr(genko::core::ApplyError("project locked: x"))), std::string("ApplyError: project locked: x"));
    }

    // A "running" job whose beat stopped (its process died) is lost, with what to do.
    void lostJobs() {
        const fs::path project = folder(QStringLiteral("jobs-c"));
        const double then = now_seconds() - 200;
        genko::storage::write_atomic(project / "studio" / "jobs" / "job_old.json",
                                     genko::core::dump_python(Json{{"id", "job_old"}, {"kind", "export"}, {"actor", "ai:x"}, {"status", "running"},
                                                                  {"started", then}, {"beat", then}, {"pid", 1}}));
        const Json status = jobs::status(project, Json("job_old"));
        QCOMPARE(status["status"], Json("lost"));
        QVERIFY(status.contains("hint"));
    }

    // Ids that are not a job's: Python's answers and its TypeError and AttributeError.
    void jobIdsAreChecked() {
        const fs::path project = folder(QStringLiteral("jobs-d"));
        for (const Json& id : {Json(""), Json("../x"), Json("a/b"), Json("a\\b"), Json(".hidden"), Json("job_none"), Json(nullptr), Json(0),
                               Json::array({"/"})}) {
            const Json status = jobs::status(project, id);
            QCOMPARE(status["ok"], Json(false));
            QCOMPARE(status["error"], Json("job " + genko::core::py_str(id) + " はない"));
        }
        QVERIFY_THROWS_EXCEPTION(genko::core::PyTypeError, jobs::status(project, Json(5)));
        QVERIFY_THROWS_EXCEPTION(genko::core::PyUncaught, jobs::status(project, Json::array({"a"})));
    }

    // adopt-drafts when studio/drafts.adopted is already there and cannot be replaced: refused before the book is saved
    // (Python saves the drafts into the book, then fails to rename the folder, and adopts them again next time).
    void adoptedDraftsAreCheckedFirst() {
        const fs::path root = folder(QStringLiteral("drafts"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"d.genko", "d", 1}), Json::object()).ok);
        const fs::path book = root / "d.genko";
        genko::storage::write_atomic(book / "studio" / "drafts" / "bible.json",
                                     genko::storage::read_file(genko::core::path_from_utf8(
                                         (genko::test::repo_root() + "/tests/fixtures/studio/demo4/bible.json").toStdString())));
        genko::storage::write_atomic(book / "studio" / "drafts.adopted" / "keep.txt", "already here");
        const std::int64_t before = genko::service::load_episode(book).revision;
        QVERIFY_THROWS_EXCEPTION(genko::core::PyUncaught, genko::service::adopt::adopt_drafts(book));
        QCOMPARE(genko::service::load_episode(book).revision, before);
        QVERIFY(fs::is_regular_file(book / "studio" / "drafts" / "bible.json"));
    }

    // import_image of a palette picture: stored as Pillow stores it, a palette PNG (its bytes against Pillow's: the
    // contract test's palette job).
    void palettePicturesAreKept() {
        const fs::path root = folder(QStringLiteral("palette"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"p.genko", "p", 1}), Json::object()).ok);
        const genko::service::ToolResult r =
            service.call("import_image", Json::array({"p.genko"}), Json{{"png_base64", genko::core::b64encode(palette_gif())}});
        QVERIFY(r.ok);
        QCOMPARE(r.data["mode"], Json("P"));
        const auto stored = genko::storage::AssetStore(root / "p.genko").get_bytes(r.data["asset"].get<std::string>(), ".png");
        QVERIFY(stored && stored->size() > 33);
        QCOMPARE(stored->substr(12, 4), std::string("IHDR"));
        QCOMPARE(static_cast<int>((*stored)[25]), 3);  // (colour type 3: a palette)
        QCOMPARE(static_cast<int>((*stored)[24]), 1);  // (two colours: one bit a pixel)
        QCOMPARE(stored->substr(37, 4), std::string("PLTE"));
    }

    // adopt: the panel's picture from the agent is decoded whole before it is placed (as any picture from elsewhere);
    // Genko's enlargement of it, written just now in this process from the pixels it made, is not decoded again when it
    // is adopted (render::vouch_for_picture: its file holds those very bytes).
    void upscaledArtIsNotDecodedAgain() {
        const fs::path root = folder(QStringLiteral("vouched"));
        genko::service::StudioService agent(root, "ai:test");
        QVERIFY(agent.call("create_project", Json::array({"v.genko", "v", 1}), Json::object()).ok);
        const fs::path book = root / "v.genko";
        const Json approved = genko::service::HumanService(book, "human:leaf").approve_name(Json::array({1}));
        QVERIFY2(approved.value("ok", false), approved.dump().c_str());
        const std::string leaf = genko::storage::load_document(book).document.page(0).leaf_frames().at(0)->id;
        const std::string art = genko::storage::AssetStore(book).put_bytes(grey_png(400, 300, 120), ".png");
        const Json imported = Json::array({Json{{"op", "import_candidates"}, {"page", 1}, {"frame_id", leaf},
                                                {"candidates", Json::array({Json{{"id", "cd_1"}, {"asset", art}, {"px", {400, 300}},
                                                                                 {"origin", Json{{"kind", "agent"}}}}})}}});
        QVERIFY(agent.call("apply_ops", Json::array({"v.genko", imported}), Json{{"commit", true}}).ok);
        const auto before = genko::render::picture_checks();
        const genko::service::ToolResult adopted =
            agent.call("adopt", Json::array({"v.genko", "cd_1"}), Json{{"page", 1}, {"frame_id", leaf}, {"preview", false}});
        QVERIFY2(adopted.ok, adopted.to_dict().dump().c_str());
        QCOMPARE(adopted.data["upscaled"], Json(true));
        const auto after = genko::render::picture_checks();
        QCOMPARE(after.decoded - before.decoded, std::uint64_t{1});  // (the agent's picture)
        QCOMPARE(after.vouched - before.vouched, std::uint64_t{1});  // (Genko's enlargement of it)
        // upscale called by itself (nothing placed next): its pixels are not kept for the drawing
        genko::render::clear_render_caches();
        const genko::service::ToolResult enlarged =
            agent.call("upscale", Json::array({"v.genko"}), Json{{"page", 1}, {"frame_id", leaf}, {"candidate_id", "cd_1"}, {"scale", 2}});
        QVERIFY2(enlarged.ok, enlarged.to_dict().dump().c_str());
        QCOMPARE(genko::render::placed_cache_pixels(), std::int64_t{0});
    }

    // Genko's enlargement adopted (with its preview): its pixels kept for the drawing are the ones the drawing decodes
    // from its file — the preview and the panel drawn from them without decoding it, the same as drawn from the file —,
    // for a picture with a transparent colour (the tRNS of an RGB or L PNG, carried by the enlargement) too (the proof
    // drawing of such pixels: test_placed keptPicturesWithATransparencyAreTheDecodedOnes).
    void keptEnlargementIsTheDecodedOne_data() {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<bool>("see_through");
        QTest::newRow("RGB") << QStringLiteral("RGB") << false;
        QTest::newRow("RGB with tRNS") << QStringLiteral("RGB") << true;
        QTest::newRow("L with tRNS") << QStringLiteral("L") << true;
        QTest::newRow("RGBA") << QStringLiteral("RGBA") << false;
    }
    void keptEnlargementIsTheDecodedOne() {
        QFETCH(QString, mode);
        QFETCH(bool, see_through);
        const std::string m = mode.toStdString();
        const fs::path root = folder(QStringLiteral("kept-") + QString::fromLatin1(QTest::currentDataTag()).replace(QLatin1Char(' '), QLatin1Char('-')));
        genko::service::StudioService agent(root, "ai:test");
        QVERIFY(agent.call("create_project", Json::array({"k.genko", "k", 1}), Json::object()).ok);
        const fs::path book = root / "k.genko";
        const Json approved = genko::service::HumanService(book, "human:leaf").approve_name(Json::array({1}));
        QVERIFY2(approved.value("ok", false), approved.dump().c_str());
        const std::string leaf = genko::storage::load_document(book).document.page(0).leaf_frames().at(0)->id;
        genko::render::Image picture = genko::render::Image::create("RGB", genko::render::Size{400, 300}, genko::render::Ink{200, 160, 120});
        picture.paste(genko::render::Ink{10, 20, 30}, genko::render::Box{60, 50, 300, 220});
        picture.paste(genko::render::Ink{90, 90, 90}, genko::render::Box{120, 100, 200, 150});
        picture = picture.convert(m);
        if (see_through) {
            genko::render::Transparency t;
            if (m == "RGB") {
                t.kind = genko::render::Transparency::Kind::Rgb;
                t.rgb = {10, 20, 30};
            } else {
                t.kind = genko::render::Transparency::Kind::Index;
                t.value = static_cast<int>(picture.getpixel(70, 60).at(0));
            }
            picture.set_transparency(t);
        }
        const std::string bytes = genko::formats::png_bytes(picture);
        QCOMPARE(bytes.find("tRNS") != std::string::npos, see_through);
        const std::string art = genko::storage::AssetStore(book).put_bytes(bytes, ".png");
        const Json imported = Json::array({Json{{"op", "import_candidates"}, {"page", 1}, {"frame_id", leaf},
                                                {"candidates", Json::array({Json{{"id", "cd_1"}, {"asset", art}, {"px", {400, 300}},
                                                                                 {"origin", Json{{"kind", "agent"}}}}})}}});
        QVERIFY(agent.call("apply_ops", Json::array({"k.genko", imported}), Json{{"commit", true}}).ok);
        genko::render::clear_render_caches();
        const genko::service::ToolResult adopted = agent.call("adopt", Json::array({"k.genko", "cd_1"}), Json{{"page", 1}, {"frame_id", leaf}});
        QVERIFY2(adopted.ok, adopted.to_dict().dump().c_str());
        QCOMPARE(adopted.data["upscaled"], Json(true));
        QCOMPARE(adopted.images.size(), std::size_t{1});
        // (the enlargement's file carries the transparency the picture had)
        const genko::core::Document adopted_book = genko::storage::load_document(book).document;
        const genko::core::Frame* frame = adopted_book.page(0).find_frame(leaf);
        QVERIFY(frame != nullptr && frame->panel && frame->panel->is_object());
        std::string enlarged_asset;
        for (const Json& c : frame->panel->value("candidates", Json::array())) {
            if (c.value("id", Json()) == adopted.data["adopted"]) enlarged_asset = c.value("asset", std::string());
        }
        QVERIFY2(!enlarged_asset.empty(), frame->panel->dump().c_str());
        const auto enlarged = genko::storage::AssetStore(book).get_bytes(enlarged_asset, ".png");
        QVERIFY(enlarged);
        QCOMPARE(enlarged->find("tRNS") != std::string::npos, see_through);
        // the panel drawn as the adopt's preview draws it: from the pixels kept, the enlargement not decoded; the same
        // as drawn from its file (the caches emptied)
        const Json args = Json{{"page", 1}, {"mode", "print"}, {"max_px", 512}, {"frame_id", leaf}};
        const auto before = genko::render::picture_checks();
        const genko::service::ToolResult kept = agent.call("render", Json::array({"k.genko"}), args);
        QVERIFY2(kept.ok, kept.to_dict().dump().c_str());
        QCOMPARE(genko::render::picture_checks().drawn_decoded - before.drawn_decoded, std::uint64_t{0});
        genko::render::clear_render_caches();
        const genko::service::ToolResult decoded = agent.call("render", Json::array({"k.genko"}), args);
        QVERIFY(decoded.ok);
        QCOMPARE(genko::render::picture_checks().drawn_decoded - before.drawn_decoded, std::uint64_t{1});
        QVERIFY(kept.images == decoded.images);
        QVERIFY(adopted.images == decoded.images);
    }

    // Genko's own enlargement (upscale.builtin), worked out in bands of rows: the same pixels and PNG bytes as one band
    // (the resize and the unsharp mask as before: render::Image::resize and filter), whatever the number of bands, for
    // each mode it takes.
    void enlargementIsTheSameInAnyNumberOfBands() {
        const genko::render::BandCounts start = genko::render::band_counts();
        genko::render::Image base = genko::render::Image::create("RGB", genko::render::Size{173, 129}, genko::render::Ink{230, 230, 230});
        for (int y = 0; y < 129; y += 7) base.paste(genko::render::Ink{40 + y, 200 - y, (y * 13) % 255}, genko::render::Box{(y * 5) % 150, y, 173, y + 3});
        for (const char* mode : {"RGB", "L", "RGBA", "LA", "1"}) {
            genko::render::Image source = base.convert(mode);
            for (const double scale : {1.0, 1.04, 2.5, 3.7}) {
                const genko::render::Image one = genko::service::upscale::builtin(source, scale, 1);
                // (one band: what the code did before it worked in bands)
                const std::string mode_used = (std::string(mode) == "RGBA" || std::string(mode) == "LA" || std::string(mode) == "P") ? "RGBA"
                                              : (std::string(mode) == "L" || std::string(mode) == "1")                          ? "L"
                                                                                                                                 : "RGB";
                const genko::render::Image plain = source.convert(mode_used);
                const genko::render::Size size{std::max(1, static_cast<int>(genko::core::py_round_int(plain.width() * scale))),
                                               std::max(1, static_cast<int>(genko::core::py_round_int(plain.height() * scale)))};
                genko::render::Image before = plain.resize(size, genko::render::Resample::Lanczos);
                if (scale > 1.05) {
                    const genko::render::Filter sharpen = genko::render::Filter::unsharp_mask(std::max(1.0, scale * 0.7), 90, 2);
                    if (mode_used == "RGBA") {
                        const genko::render::Image rgb = before.convert("RGB").filter(sharpen);
                        const genko::render::Image alpha = before.getchannel("A");
                        before = rgb.convert("RGBA");
                        before.putalpha(alpha);
                    } else {
                        before = before.filter(sharpen);
                    }
                }
                QVERIFY2(one.tobytes() == before.tobytes(), (std::string(mode) + " ×" + std::to_string(scale)).c_str());
                for (const int threads : {2, 3, 4, 7, 16}) {
                    const genko::render::Image banded = genko::service::upscale::builtin(source, scale, threads);
                    const std::string what = std::string(mode) + " ×" + std::to_string(scale) + " in " + std::to_string(threads) + " bands";
                    QVERIFY2(banded.mode() == one.mode() && banded.size() == one.size() && banded.tobytes() == one.tobytes(), what.c_str());
                    QVERIFY2(genko::formats::png_bytes(banded) == genko::formats::png_bytes(one), what.c_str());
                }
            }
        }
        const genko::render::BandCounts end = genko::render::band_counts();
        QCOMPARE(end.failed - start.failed, std::uint64_t{0});
        QVERIFY(end.banded - start.banded > 50);
    }

    // formats::run's official export, handed to the studio by service::run_export after Python's own refusals: the
    // studio's preflight answers (not "not yet ported").
    void officialExportGoesThroughTheStudio() {
        const fs::path root = folder(QStringLiteral("official"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"o.genko", "o", 1}), Json{{"spec_preset", "doujin-a5"}}).ok);
        const fs::path book = root / "o.genko";
        const genko::core::Document doc = genko::service::load_episode(book);
        genko::formats::RunOptions options;
        options.official = true;
        options.actor = "human:leaf";
        const Json studio = genko::service::run_export(doc, book, "pdf", root / "out", options);
        QCOMPARE(studio["ok"], Json(false));
        QCOMPARE(studio["error"], Json("preflight で止まった"));
        QVERIFY(studio["errors"].is_array() && !studio["errors"].empty());
        QVERIFY(!fs::exists(root / "out"));
        // (Python's own refusals come first)
        const Json layers = genko::service::run_export(doc, book, "layers", root / "out", options);
        QVERIFY(layers["error"].get<std::string>().ends_with("は正式な書き出しに使えない"));
        // an agent cannot make it
        options.actor = "ai:agent";
        QVERIFY_THROWS_EXCEPTION(genko::core::ApplyError, genko::service::run_export(doc, book, "pdf", root / "out", options));
        // without the studio, formats::run alone still refuses it
        options.actor = "human:leaf";
        const Json alone = genko::formats::run(doc, book, "pdf", root / "out", options);
        QCOMPARE(alone["code"], Json("not_yet_ported"));
    }

    // The ops an agent may send (AGENT_OPS) and the files they may read (in the book or under --root) are the same
    // inside for_pages: put_raster (which reads any file) and import_psd outside the root are refused there too, and
    // nothing changes. (Python checks only the ops given at the top.)
    void forPagesKeepsTheAgentsRules() {
        const fs::path root = folder(QStringLiteral("forpages/root"));
        const fs::path outside = folder(QStringLiteral("forpages/outside"));
        genko::storage::write_atomic(outside / "secret.png", grey_png(8, 8, 90));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"f.genko", "f", 2}), Json::object()).ok);
        const auto before = genko::test::tree_hashes(q(root));
        const std::string secret = genko::core::path_to_utf8(outside / "secret.png");
        const auto refused = [&](const Json& inner, bool commit) {
            const Json ops = Json::array({Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({inner})}}});
            return service.call("apply_ops", Json::array({"f.genko", ops}), Json{{"commit", commit}});
        };
        for (const bool commit : {false, true}) {
            const genko::service::ToolResult raster = refused(Json{{"op", "put_raster"}, {"path", secret}}, commit);
            QVERIFY(!raster.ok);
            QCOMPARE(raster.issues.at(0).code, std::string("op_not_allowed"));
            QCOMPARE(raster.issues.at(0).path, std::string("/ops/0/ops/0/op"));
            const genko::service::ToolResult psd = refused(Json{{"op", "import_psd"}, {"path", secret}}, commit);
            QVERIFY(!psd.ok);
            QCOMPARE(psd.issues.at(0).code, std::string("path_outside_root"));
            QCOMPARE(psd.issues.at(0).path, std::string("/ops/0/ops/0/path"));
            const genko::service::ToolResult relative = refused(Json{{"op", "import_psd"}, {"path", "../../outside/secret.png"}}, commit);
            QCOMPARE(relative.issues.at(0).code, std::string("path_outside_root"));
            // (a second for_pages, inside the first: its ops checked as well)
            const Json nested = Json{{"op", "for_pages"}, {"ops", Json::array({Json{{"op", "approve"}, {"gate", "name"}}})}};
            const genko::service::ToolResult deeper = refused(nested, commit);
            QVERIFY(!deeper.ok);
            QCOMPARE(deeper.issues.at(0).code, std::string("op_not_allowed"));
            QCOMPARE(deeper.issues.at(0).path, std::string("/ops/0/ops/0/ops/0/op"));
        }
        QVERIFY(genko::test::tree_hashes(q(root)) == before);
        // what an agent may do page by page still goes through
        const Json notes = Json::array({Json{{"op", "for_pages"}, {"pages", "all"}, {"ops", Json::array({Json{{"op", "set_note"}, {"note", "x"}}})}}});
        QVERIFY(service.call("apply_ops", Json::array({"f.genko", notes}), Json{{"commit", true}}).ok);
    }

    // put_raster's path (a person's `genko apply`, or what a plugin sends): only a regular file is read (not a device or
    // a pipe, which may never end), and none past 2 GiB. (Python reads whatever the path gives, whole.)
    void putRasterReadsRegularFilesOnly() {
        const fs::path book = plain_book(folder(QStringLiteral("raster/r.genko")), 1);
        const genko::core::Document doc = genko::service::load_episode(book);
        const auto refusal = [&](const std::string& path) -> std::string {
            try {
                genko::service::apply_ops(doc, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"path", path}}}), "human:leaf", true);
            } catch (const genko::core::PyUncaught& error) {
                return error.type() + ": " + error.what();
            } catch (const std::exception& error) {
                return error.what();
            }
            return "read";
        };
        QVERIFY2(refusal("/dev/null").find("the file cannot be read (it is not a regular file)") != std::string::npos, refusal("/dev/null").c_str());
        QVERIFY(refusal("/dev/zero").find("the file cannot be read (it is not a regular file)") != std::string::npos);
        // (Python's own errors for a folder and a missing file)
        QVERIFY(refusal(genko::core::path_to_utf8(book)).find("IsADirectoryError") != std::string::npos);
        QVERIFY(refusal(genko::core::path_to_utf8(book / "none.png")).find("FileNotFoundError") != std::string::npos);
        // a picture is read as before
        genko::storage::write_atomic(book / "p.png", grey_png(4, 4, 30));
        QCOMPARE(refusal(genko::core::path_to_utf8(book / "p.png")), std::string("read"));
    }

    void putRasterRefusesHugeFiles() {
        const fs::path book = plain_book(folder(QStringLiteral("raster-huge/r.genko")), 1);
        const fs::path huge = book / "huge.png";
        {
            QFile file(q(huge));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize((qint64{1} << 31) + 1));  // (sparse: nothing written)
        }
        const genko::core::Document doc = genko::service::load_episode(book);
        const auto started = std::chrono::steady_clock::now();
        std::string said = "read";
        try {
            genko::service::apply_ops(doc, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"path", genko::core::path_to_utf8(huge)}}}), "human:leaf", true);
        } catch (const std::exception& error) {
            said = error.what();
        }
        QVERIFY2(said.find("the file cannot be read (it is larger than 2 GiB)") != std::string::npos, said.c_str());
        QVERIFY(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));  // (refused before it is read)
        fs::remove(huge);
    }

    // A preview's file name made of an id (a candidate's, a frame's, a layer's): the id's "/" and "\\" written "_", so
    // the file stays in studio/reviews/ (an ordinary id keeps Python's name: the contract test).
    void previewNamesStayInTheBook() {
        const fs::path root = folder(QStringLiteral("names/root"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"n.genko", "n", 1}), Json{{"spec_preset", "doujin-a5"}}).ok);
        const fs::path book = root / "n.genko";
        const std::string ref = genko::storage::AssetStore(book).put_bytes(grey_png(40, 30, 120), ".png");
        const std::string frame = genko::service::load_episode(book).page(0).leaf_frames().at(0)->id;
        const std::string id = "../../../../../escaped";
        const Json candidates = Json::array({Json{{"op", "import_candidates"}, {"page", 1}, {"frame_id", frame},
                                                  {"candidates", Json::array({Json{{"id", id}, {"asset", ref}, {"px", Json::array({40, 30})},
                                                                                   {"origin", Json{{"kind", "agent"}}}}})}}});
        QVERIFY(service.call("apply_ops", Json::array({"n.genko", candidates}), Json{{"commit", true}}).ok);
        const genko::service::ToolResult shown =
            service.call("render", Json::array({"n.genko", 1}), Json{{"kind", "compare"}, {"frame_id", frame}, {"candidate_id", id}, {"max_px", 64}});
        QVERIFY(shown.ok);
        QCOMPARE(shown.files.size(), std::size_t{1});
        const fs::path written = genko::core::path_from_utf8(shown.files[0]);
        QCOMPARE(written.parent_path(), book / "studio" / "reviews");
        QCOMPARE(genko::core::path_to_utf8(written.filename()), "p001_compare_" + frame + "_.._.._.._.._.._escaped.png");
        QVERIFY(fs::is_regular_file(written));
        std::error_code ec;
        for (fs::recursive_directory_iterator it(folder(QStringLiteral("names")), ec), end; !ec && it != end; it.increment(ec)) {
            QVERIFY2(genko::core::path_to_utf8(it->path().filename()).find("escaped") == std::string::npos || it->path() == written,
                     genko::core::path_to_utf8(it->path()).c_str());
        }
    }

    // A page another writer takes away between a tool's own reads: the tool says the page is not there (Python's
    // next(…) stops with a StopIteration) instead of reading a page that is not there.
    void pagesGoneMeanwhile() {
        const fs::path root = folder(QStringLiteral("gone"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"g.genko", "g", 2}), Json{{"spec_preset", "doujin-a5"}}).ok);
        const fs::path book = root / "g.genko";
        genko::storage::write_atomic(root / "scans" / "p.png", scan_png());
        QVERIFY(service.call("import_name", Json::array({"g.genko", Json::array({"scans/p.png"})}), Json{{"start_page", 2}}).ok);
        const auto take_page_two = [&book] { as_person(book, Json::array({Json{{"op", "delete_page"}, {"page", 2}}})); };
        // analyze_name: page 2 goes after its proposal is saved, while the tool reads the book again for the overlay
        {
            const std::int64_t saved = revision_of(book);
            const IdHook hook([&] { return revision_of(book) > saved; }, take_page_two);
            genko::service::ToolResult r;
            try {
                r = service.call("analyze_name", Json::array({"g.genko", 2}), Json::object());
            } catch (const std::exception& error) {
                QFAIL(error.what());
            }
            hook.check();
            QVERIFY(hook.fired());
            QVERIFY(!r.ok);
            QCOMPARE(r.issues.at(0).code, std::string("no_page"));
        }
        // finish_page: page 2 goes after the tool read it and before it saves the finishing pass
        QVERIFY(service.call("create_project", Json::array({"h.genko", "h", 2}), Json{{"spec_preset", "doujin-a5"}}).ok);
        const fs::path second = root / "h.genko";
        genko::service::HumanService human(second, "human:leaf");
        human.approve_name(Json::array({2}));
        human.approve_art(Json::array({2}));
        {
            const IdHook hook([] { return true; }, [&second] { as_person(second, Json::array({Json{{"op", "delete_page"}, {"page", 2}}})); });
            genko::service::ToolResult r;
            try {
                r = service.call("finish_page", Json::array({"h.genko", 2}), Json{{"commit", true}});
            } catch (const std::exception& error) {
                QFAIL(error.what());
            }
            hook.check();
            QVERIFY(hook.fired());
            QVERIFY(!r.ok);
            QCOMPARE(r.issues.at(0).code, std::string("no_page"));
            QCOMPARE(genko::service::load_episode(second).pages.size(), std::size_t{1});
            QCOMPARE(genko::service::load_episode(second).page(0).stage, std::string("name"));
        }
    }

    // A book of v1–v3 (Python's): the tools that write refuse it before they write anything — the book, its assets,
    // its requests, a preview, even a dry run's — with needs_migration (exit 3 from `genko studio`, as from every genko
    // command); the tools that read answer as Python's.
    void oldBooksAreNotWritten() {
        const fs::path root = folder(QStringLiteral("old"));
        genko::test::copy_tree(genko::test::test_data(QStringLiteral("legacy/book-v3.genko")), q(root / "old.genko"));
        const auto before = genko::test::tree_hashes(q(root));
        genko::service::StudioService service(root);
        const Json book("old.genko");
        const std::string png = genko::core::b64encode(grey_png(10, 10, 60));
        const std::vector<std::pair<std::string, Json>> writing = writing_calls(png);
        for (const auto& [tool, kwargs] : writing) {
            Json all = kwargs;
            all["project"] = book;
            QVERIFY2(code_thrown([&] { service.call(tool, Json::array(), all); }) == "needs_migration", tool.c_str());
        }
        QVERIFY(code_thrown([&] { genko::service::HumanService(root / "old.genko", "human:leaf").approve_name(Json::array({1})); }) == "needs_migration");
        QVERIFY(code_thrown([&] { genko::service::HumanService(root / "old.genko", "human:leaf").chat_approval(Json(false)); }) == "needs_migration");
        QVERIFY(code_thrown([&] { genko::service::adopt::adopt_drafts(root / "old.genko"); }) == "needs_migration");
        QVERIFY(genko::test::tree_hashes(q(root)) == before);  // (no project.lock made either)
        QVERIFY(service.call("status", Json::array({book}), Json::object()).ok);  // (reading is fine)
        // the command line: exit 3, the code said
        genko::test::write_bytes(q(root.parent_path() / "old-ops.json"), R"([{"op": "set_note", "page": 1, "note": "x"}])");
        const QString old_book = q(root / "old.genko");
        for (const QStringList& args : {QStringList{"studio", "apply", old_book, q(root.parent_path() / "old-ops.json")},
                                        QStringList{"studio", "apply", old_book, q(root.parent_path() / "old-ops.json"), "--commit"},
                                        QStringList{"studio", "approve", old_book, "name", "--pages", "1", "--as", "human:leaf"},
                                        QStringList{"studio", "adopt-drafts", old_book}}) {
            const auto ran = genko::test::run_genko(args);
            QVERIFY2(ran.exit_code == 3, qPrintable(args.join(' ') + ": " + ran.described()));
            QCOMPARE(genko::test::one_line(ran.out)["code"], Json("needs_migration"));
        }
        QCOMPARE(genko::test::run_genko({"studio", "status", old_book}).exit_code, 0);
        QVERIFY(genko::test::tree_hashes(q(root)) == before);
    }

    // request_approval for several character sheets (character_ids): every request made in one save (one revision);
    // one that cannot be made leaves the book as it was.
    void characterRequestsAreOneSave() {
        const fs::path root = folder(QStringLiteral("sheets"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"s.genko", "s", 1}), Json::object()).ok);
        const Json bible = genko::core::parse_python_json(genko::storage::read_file(genko::core::path_from_utf8(
            (genko::test::repo_root() + "/tests/fixtures/studio/demo4/bible.json").toStdString())));
        QVERIFY(service.call("set_bible", Json::array({"s.genko", bible}), Json{{"commit", true}}).ok);
        const fs::path book = root / "s.genko";
        const std::int64_t before = revision_of(book);
        const genko::service::ToolResult asked =
            service.call("request_approval", Json::array({"s.genko", "sheet", Json::array()}), Json{{"character_ids", Json::array({"hina", "sora", "hina"})}});
        QVERIFY(asked.ok);
        QCOMPARE(asked.data["requests"].size(), std::size_t{2});
        QCOMPARE(revision_of(book), before + 1);
        const genko::core::Document doc = genko::service::load_episode(book);
        QCOMPARE(doc.tickets.size(), std::size_t{2});
        QCOMPARE(asked.data["requests"][0]["request"], doc.tickets[0]["id"]);
        QCOMPARE(asked.data["requests"][1]["request"], doc.tickets[1]["id"]);
        // one that cannot be made: nothing saved
        bool refused = false;
        try {
            refused = !service.call("request_approval", Json::array({"s.genko", "sheet", Json::array()}), Json{{"character_ids", Json::array({"sora", ""})}}).ok;
        } catch (const std::exception&) {
            refused = true;
        }
        QVERIFY(refused);
        QCOMPARE(revision_of(book), before + 1);
    }

    // A person stops chat approvals while an agent records one: the agent's record is checked again under the book's
    // lock, so it is refused (Python looks once, before it takes the lock).
    void chatApprovalIsCheckedUnderTheLock() {
        const fs::path root = folder(QStringLiteral("chat"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"c.genko", "c", 1}), Json::object()).ok);
        const fs::path book = root / "c.genko";
        genko::service::ToolResult r;
        {
            const IdHook hook([] { return true; }, [&book] { genko::service::HumanService(book, "human:leaf").chat_approval(Json(false)); });
            r = service.call("record_chat_approval", Json::array({"c.genko", "name", "OK です"}), Json{{"pages", Json::array({1})}});
            hook.check();
            QVERIFY(hook.fired());
        }
        QVERIFY(!r.ok);
        QCOMPARE(r.issues.at(0).code, std::string("chat_approval_off"));
        QVERIFY(!genko::service::load_episode(book).page(0).name_ok);
    }

    // The official export holds the book from its read to its approval: another writer cannot change the book between
    // what was written and what was approved; when the approval cannot be saved, what was written is taken back.
    void officialExportHoldsTheBook() {
        const fs::path book = plain_book(folder(QStringLiteral("hold/h.genko")), 1, "finish");
        genko::service::OfficialExport options;
        options.dpi = 20;
        {
            bool locked = false;
            bool changed = false;
            const IdHook hook([] { return true; }, [&] {
                try {
                    as_person(book, Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "meanwhile"}}}));
                    changed = true;
                } catch (const genko::core::ApplyError& error) {
                    locked = error.code() == "locked";
                }
            });
            const Json reply = genko::service::HumanService(book, "human:leaf").export_book("png", book.parent_path() / "out", options);
            hook.check();
            QVERIFY(hook.fired());
            QCOMPARE(reply["ok"], Json(true));
            QVERIFY(locked);
            QVERIFY(!changed);
        }
    }

    // The official export's approval cannot be saved (the book changed under it, by a writer that did not wait for the
    // lock): the pages written are taken back, and the error said.
    void officialExportTakesBackItsPages() {
        genko::service::OfficialExport options;
        options.dpi = 20;
        const fs::path other = plain_book(folder(QStringLiteral("hold/k.genko")), 1, "finish");
        const fs::path out = other.parent_path() / "out2";
        const IdHook hook([&out] { return fs::exists(out) && !fs::is_empty(out); }, [&other] {
            Json payload = genko::core::parse_python_json(genko::storage::read_file(other / "project.json"));
            payload["revision"] = payload["revision"].get<std::int64_t>() + 1;
            genko::storage::write_atomic(other / "project.json", genko::core::dump_python(payload));
        });
        const std::string thrown = code_thrown([&] { genko::service::HumanService(other, "human:leaf").export_book("png", out, options); });
        hook.check();
        QVERIFY(hook.fired());
        QCOMPARE(thrown, std::string("revision_conflict"));
        QCOMPARE(files_under(out), std::size_t{0});
    }

    // The official export to print with the dots a person chose (screen): the greys printed as dots (Python's HumanService
    // overwrites screen with whether the format is for screens, so it never prints them).
    void officialExportPrintsTheDots() {
        const std::string grey = genko::core::b64encode(grey_png(200, 200, 128));
        // (two books the same: an official export records its approval, and a book with approvals is a studio book,
        // which the next export's preflight asks more of)
        const auto made = [&](const QString& name) {
            const fs::path book = plain_book(folder(name), 1, "finish");
            as_person(book, Json::array({Json{{"op", "put_raster"}, {"page", 1}, {"png_base64", grey}, {"layer", "ink"}}}));
            return book;
        };
        const fs::path first = made(QStringLiteral("dots/a.genko"));
        const fs::path second = made(QStringLiteral("dots/b.genko"));
        genko::service::OfficialExport plain;
        plain.dpi = 72;
        plain.color = "bitonal";
        genko::service::OfficialExport dotted = plain;
        dotted.screen = Json{{"lpi", 20}, {"shape", "round"}};
        const Json a = genko::service::HumanService(first, "human:leaf").export_book("png", first.parent_path() / "plain", plain);
        const Json b = genko::service::HumanService(second, "human:leaf").export_book("png", second.parent_path() / "dotted", dotted);
        QVERIFY2(a["ok"] == Json(true), a.dump().c_str());
        QVERIFY2(b["ok"] == Json(true), b.dump().c_str());
        const std::string one = genko::test::read_bytes(QString::fromStdString(a["files"][0].get<std::string>()));
        const std::string two = genko::test::read_bytes(QString::fromStdString(b["files"][0].get<std::string>()));
        QVERIFY(!one.empty() && !two.empty());
        QVERIFY(one != two);
    }

    // Another process holds the book's lock: the writing tools refuse (project locked), create_project does not write a
    // book there, and nothing changes.
    void anotherProcessHoldsTheLock() {
        const fs::path root = folder(QStringLiteral("held"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"l.genko", "l", 1}), Json::object()).ok);
        genko::test::Holder holder;
        QCOMPARE(holder.start(genko::test::lock_helper(), {q(root / "l.genko"), "human:other"}), QStringLiteral("locked"));
        const auto before = genko::test::tree_hashes(q(root), false);
        const Json ops = Json::array({Json{{"op", "set_note"}, {"page", 1}, {"note", "x"}}});
        // (as Python's: the lock's ApplyError is the tool's apply_failed)
        const genko::service::ToolResult applied = service.call("apply_ops", Json::array({"l.genko", ops}), Json{{"commit", true}});
        QVERIFY(!applied.ok);
        QCOMPARE(applied.issues.at(0).code, std::string("apply_failed"));
        QCOMPARE(code_thrown([&] { genko::service::HumanService(root / "l.genko", "human:leaf").approve_name(Json::array({1})); }), std::string("locked"));
        QVERIFY(genko::test::tree_hashes(q(root), false) == before);
        holder.stop();
        // create_project where another process holds the folder's lock: no book is written
        fs::create_directories(root / "m.genko");
        genko::test::Holder second;
        QCOMPARE(second.start(genko::test::lock_helper(), {q(root / "m.genko"), "human:other"}), QStringLiteral("locked"));
        QCOMPARE(code_thrown([&] { service.call("create_project", Json::array({"m.genko", "m", 1}), Json::object()); }), std::string("locked"));
        QVERIFY(!fs::exists(root / "m.genko" / "project.json"));
        second.stop();
    }

    // confine (read files outside --root) is Python's keyword-only argument that only its command line passes: callers
    // of StudioService::call (MCP, HTTP) cannot pass it; the command line sets it on its service.
    void confineIsNotAnArgument() {
        const fs::path root = folder(QStringLiteral("confine/root"));
        genko::storage::write_atomic(folder(QStringLiteral("confine/outside")) / "o.png", grey_png(6, 6, 10));
        const std::string outside = genko::core::path_to_utf8(folder(QStringLiteral("confine/outside")) / "o.png");
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"c.genko", "c", 1}), Json::object()).ok);
        for (const std::string tool : {"import_image", "import_name"}) {
            bool refused = false;
            try {
                const Json kwargs = tool == "import_image" ? Json{{"path", outside}, {"confine", false}} : Json{{"files", Json::array({outside})}, {"confine", false}};
                service.call(tool, Json::array({"c.genko"}), kwargs);
            } catch (const genko::core::PyTypeError& error) {
                refused = std::string(error.what()).find("unexpected keyword argument 'confine'") != std::string::npos;
            } catch (const std::exception&) {
            }
            QVERIFY2(refused, tool.c_str());
        }
        bool has = false;
        for (const genko::service::Param& p : genko::service::StudioService::parameters("import_image")) has = has || std::string_view(p.name) == "confine";
        QVERIFY(!has);
        const genko::service::ToolResult confined = service.call("import_image", Json::array({"c.genko"}), Json{{"path", outside}});
        QCOMPARE(confined.issues.at(0).code, std::string("outside_root"));
        service.set_confine(false);
        QVERIFY(service.call("import_image", Json::array({"c.genko"}), Json{{"path", outside}}).ok);
    }

    // A job whose worker thread cannot start: the job says it failed and nothing is left waiting (the beat ends), so
    // wait_all() returns; Python's RuntimeError ("can't start new thread") is what the tool throws.
    void jobsWhoseThreadCannotStart() {
        const fs::path project = folder(QStringLiteral("jobs-e"));
        std::string thrown;
        {
            const jobs::ScopedThreadFailure failing(2);  // (the beat starts, the worker does not)
            try {
                jobs::start(project, "export", [] { return Json{{"ok", true}}; }, "ai:test", 0.05);
            } catch (const genko::core::PyUncaught& error) {
                thrown = error.type() + ": " + error.what();
            }
        }
        QCOMPARE(thrown, std::string("RuntimeError: can't start new thread"));
        std::promise<void> waited;
        std::future<void> done = waited.get_future();
        std::thread([&waited] {
            jobs::wait_all();
            waited.set_value();
        }).detach();
        QVERIFY(done.wait_for(std::chrono::seconds(20)) == std::future_status::ready);
        // the record: failed, with the error
        Json record;
        for (const auto& entry : fs::directory_iterator(project / "studio" / "jobs")) {
            record = genko::core::parse_python_json(genko::storage::read_file(entry.path()));
        }
        QCOMPARE(record["status"], Json("failed"));
        QCOMPARE(record["result"]["error"], Json("RuntimeError: can't start new thread"));
        // and the next job starts as usual
        const Json next = jobs::start(project, "export", [] { return Json{{"ok", true}}; }, "ai:test", 30);
        QCOMPARE(next["ok"], Json(true));
        jobs::wait_all();
    }

    // The command line's own refusals carry a code as its other failures do; a range of pages past 64 bits, or one too
    // long to list, is refused (Python builds the list until memory runs out); a width that does not fit in an int is
    // refused (not cut to its low bits).
    void commandLineRefusals() {
        const fs::path root = folder(QStringLiteral("cli"));
        genko::service::StudioService service(root);
        QVERIFY(service.call("create_project", Json::array({"k.genko", "k", 1}), Json::object()).ok);
        const QString book = q(root / "k.genko");
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), q(root / "config"));
        const auto ran = [&env](const QStringList& args) { return genko::test::run_genko(QStringList{"studio"} + args, env); };
        for (const QStringList& args : {QStringList{"upscaler", "add", "x"}, QStringList{"tools", "set"}, QStringList{"call", book, "nope"},
                                        QStringList{"approve", book, "sheet", "--as", "human:leaf"}}) {
            const auto r = ran(args);
            const Json out = genko::test::one_line(r.out);
            QVERIFY2(r.exit_code == 1 && out.is_object() && out.value("ok", true) == false && out.contains("code"), qPrintable(args.join(' ') + ": " + r.described()));
        }
        for (const QString& pages : {QStringLiteral("1-9223372036854775807"), QStringLiteral("1-20000000")}) {
            const auto r = ran({"approve", book, "name", "--pages", pages, "--as", "human:leaf"});
            const Json out = genko::test::one_line(r.out);
            QVERIFY2(r.exit_code == 1 && out.is_object() && out.value("code", std::string()) == "pages_too_many", qPrintable(r.described()));
        }
        const genko::service::ToolResult wide =
            service.call("export", Json::array({"k.genko"}), Json{{"format", "webtoon"}, {"width", 4294967396LL}, {"max_height", 300}});
        QVERIFY(!wide.ok);
        QCOMPARE(wide.issues.at(0).path, std::string("/width"));
        QVERIFY(!fs::exists(root / "k.genko" / "exports"));
    }

    // The tests' fixed clock is a time, written as each name asks: a job's id, an export's folder have no ":" (which
    // Windows takes in no name); a text that is no such time is given as it is (tools/migration/studio_harness.py's
    // fixed_clock does the same).
    void fixedClockWritesTheTimeAsAsked() {
        {
            const genko::core::ScopedClock clock("2026-10-01T09:00:00");
            QCOMPARE(genko::core::local_time_text(), std::string("2026-10-01T09:00:00"));
            QCOMPARE(genko::core::local_time_text("%Y%m%d-%H%M%S"), std::string("20261001-090000"));
            QCOMPARE(genko::core::utc_time_text("%Y%m%d%H%M%SZ"), std::string("20261001090000Z"));
            QCOMPARE(genko::core::local_time_text("%a %j"), std::string("Thu 274"));
            const Json job = jobs::start(folder(QStringLiteral("clock")), "export", [] { return Json{{"ok", true}}; }, "ai:test", 30);
            QVERIFY(job["job"].get<std::string>().starts_with("job_20261001-090000_"));
            jobs::wait_all();
        }
        const genko::core::ScopedClock other("not a time");
        QCOMPARE(genko::core::local_time_text("%Y%m%d"), std::string("not a time"));
    }

    // A proof's PDF (export_proof, Pillow's PDF writer) is the same whatever the process's locale writes numbers with: its
    // page's drawing command has C's decimal point (C's printf takes the locale's: a German locale writes "1,5").
    void pdfNumbersIgnoreTheLocale() {
        const std::string was = std::setlocale(LC_NUMERIC, nullptr);
        const auto comma_locale = []() -> bool {
            for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "de-DE", "fr_FR.UTF-8", "fr_FR.utf8", "nl_NL.UTF-8"}) {
                if (std::setlocale(LC_NUMERIC, name) != nullptr && std::localeconv()->decimal_point[0] == ',') return true;
            }
            return false;
        };
        bool found = comma_locale();
#ifdef __linux__
        QTemporaryDir made;
        if (!found && made.isValid()) {  // (a German locale made for the test when the system has none: glibc's LOCPATH)
            QProcess::execute(QStringLiteral("localedef"), {QStringLiteral("-i"), QStringLiteral("de_DE"), QStringLiteral("-f"), QStringLiteral("UTF-8"),
                                                              made.filePath(QStringLiteral("de_DE.UTF-8"))});
            qputenv("LOCPATH", QFile::encodeName(made.path()));
            found = comma_locale();
            qunsetenv("LOCPATH");
        }
#endif
        if (!found) {
            std::setlocale(LC_NUMERIC, was.c_str());
            QSKIP("no locale here writes numbers with a decimal comma");
        }
        const std::vector<genko::render::Image> pages = {genko::render::Image::create("L", genko::render::Size{30, 45}, genko::render::Ink(128))};
        const std::string comma = genko::formats::pdf_bytes(pages, 72.0, "proof", "20261001090000Z");
        std::setlocale(LC_NUMERIC, "C");
        const std::string point = genko::formats::pdf_bytes(pages, 72.0, "proof", "20261001090000Z");
        std::setlocale(LC_NUMERIC, was.c_str());
        QVERIFY(point.find("q 30.000000 0 0 45.000000 0 0 cm /image Do Q") != std::string::npos);
        QVERIFY(comma == point);
    }
};

QTEST_GUILESS_MAIN(TestService)
#include "test_service.moc"
