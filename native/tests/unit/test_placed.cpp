// Placed pictures (M5) where this build goes past Python or must not stop on what it is given: the size of an asset's
// picture read from its header only, as Image.open reads it (a PNG, JPEG, BMP or GIF whose pixels are cut short has a
// size; decoding it fails, as Python's load() does); such a picture not placed — by place_asset, adopt_candidate or
// set_placement — the whole picture decoded first (C++ only: Python places it and the drawing fails), the batch refused;
// one that came in without being placed here (a book written elsewhere) stopping the drawing and the export with an
// error that says so, nothing written; an asset file past what the store reads refused (C++ only); a digest naming
// another place no asset (C++ only); a WebP not opened yet (not_yet_ported); a finish whose screen is too coarse for the
// resolution refused (C++ only: Python takes minutes and gigabytes); and the pictures remembered between tiles let go.
// (The drawing against Python's: test_contract_studio's placedPicturesLikePython; the refusals through the command
// line: its brokenPicturesAreNotPlaced.)

#include <QtTest>

#include <QDir>
#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/paths.hpp"
#include "formats/exporting.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/placed.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::ApplyError;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
namespace fs = std::filesystem;
namespace render = genko::render;

namespace {

// 9×6 pictures as Pillow writes them, and the same cut short in their pixels: Image.open(…).size is (9, 6) for each,
// load() fails on the cut ones ("image file is truncated"; a JPEG, a BMP and a GIF are read by their header and their
// pixels apart, a PNG's chunks before its pixels the same).
struct Sample {
    const char* name;
    const char* whole;
    const char* cut;
};
const Sample kSamples[] = {
    {"JPEG",
     "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAGAAkDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDz+4WTTle4hkLRZG6KQk9TyQfXmmf27a/885vyH+NFFevhMLRxuChiMRHmneSvs2ko2va13ru9fM+qr16mGxMqVF2jZO3re9r7ei0P/9k=",
     "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAAGAAkDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDz+w=="},
    {"BMP",
     "Qk3eAAAAAAAAADYAAAAoAAAACQAAAAYAAAABABgAAAAAAKgAAADEDgAAxA4AAAAAAAAAAAAAHh7IHh7IHh7IHh7IHh7IHh7I/wAA/wAA/wAAAB4eyB4eyB4eyB4eyB4eyP8AAP8AAP8AAB4eyAAeHsgeHsgeHsj/AAD/AAD/AAD/AAAeHsgeHsgAHh7IHh7I/wAA/wAA/wAAHh7IHh7IHh7IHh7IAP8AAP8AAP8AAP8AAB4eyB4eyB4eyB4eyB4eyAAeHsj/AAAeHsgeHsgeHsgeHsgeHsgeHsgeHsgA",
     "Qk3eAAAAAAAAADYAAAAoAAAACQAAAAYAAAABABgAAAAAAKgAAADEDgAAxA4AAAAAAAAAAAAAHh7IHh7IHh7IHh7IHh7IHh7I/wAA/wAA/wAAAB4eyB4eyB4eyB4eyB4eyP8AAP8AAP8AAB4eyAAeHsgeHsgeHsj/AAD/AAD/AAD/AAAeHsgeHsgAHh7IHh7I/wAA/wAA/wAAHh7IHh7IHh7IHh7IAP8AAP8AAP8AAP8AAB4eyB4="},
    {"GIF", "R0lGODdhCQAGAIIAAMwAAMwzAMwAM8wzMwAA/wAAAAAAAAAAACwAAAAACQAGAAAIIgAHEAgggKBBAggJDCC4cACAhAULMkyoEIDDiwkHXDyIMCAAOw==",
     "R0lGODdhCQAGAIIAAMwAAMwzAMwAM8wzMwAA/wAAAAAAAAAAACwAAAAACQAGAAAIIgAHEAgggKBBAggJDCC4cACAhAULMkyoEA=="},
    {"PNG",
     "iVBORw0KGgoAAAANSUhEUgAAAAkAAAAGCAIAAACepSOSAAAAMklEQVR4nGNkYGCQw4FYGDQYGBgEsSK4nBImQpYzRkUdaHIuDAwuDAxpEAamXCgMdQAAPsEJHEGEH+8AAAAASUVORK5CYII=",
     "iVBORw0KGgoAAAANSUhEUgAAAAkAAAAGCAIAAACepSOSAAAAMklEQVR4nGNkYGCQw4FYGDQYGBgEsSK4nBImQpYz"},
};

// A WebP, 2×2, as Pillow writes one (this build does not open a WebP yet).
const char* const kWebp = "UklGRh4AAABXRUJQVlA4TBEAAAAvAUAAAAdQhSJXof+BiOh/AAA=";

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(render::ops_registry());
    return b;
}

// A two-page book in `dir` (asset_dir), with page 1's name approved (art can be adopted there).
Document book_in(const QString& dir) {
    Document doc = genko::core::new_episode("配置", Num(1), 2, genko::core::PageSpec::a4_mono());
    doc.asset_dir = genko::core::path_from_utf8(dir.toStdString());
    doc.edit_page(0).name_ok = true;
    return doc;
}

std::string put(const QString& dir, const std::string& bytes) {
    return genko::storage::AssetStore(genko::core::path_from_utf8(dir.toStdString())).put_bytes(bytes, ".png");
}

// A placed picture of `ref` on page 1's first panel as a book written elsewhere has it (by Python, which places a
// picture whose pixels are cut short, or an older build): no candidate, so set_placement reads the picture's size.
Document with_placed(Document doc, const std::string& ref) {
    genko::core::Layer layer;
    layer.id = "ly_placed";
    layer.role = genko::core::LayerRole::User;
    layer.kind = genko::core::LayerKind::Placed;
    layer.exportable = true;
    layer.title = "placed art";
    layer.frame_id = doc.page(0).frames[0].id;
    layer.asset = ref;
    layer.source = Json::object({{"asset", ref}, {"to", "art"}, {"by", "human:作者"}});
    layer.placement_mm = genko::core::Rect{Num(30), Num(40), Num(90), Num(60)};
    layer.fit = "stretch";
    layer.clip_to = "frame";
    auto& page = doc.edit_page(0);
    page.layers.insert(page.layers.begin(), std::move(layer));
    return doc;
}

// The batch refused: its error's code and words ("(applied)" when it is not).
std::string refused(const Document& doc, const Json& batch) {
    try {
        (void)bus().apply(doc, batch, Actor("human:作者"));
    } catch (const ApplyError& e) {
        return e.code() + ": " + e.what();
    }
    return "(applied)";
}

// What drawing the page says (its error's code and words), or "(drawn)".
std::string drawing_says(const Document& doc, int dpi = 60, const std::string& mode = "print") {
    render::clear_render_caches();
    render::RenderOptions options;
    options.mode = mode;
    try {
        (void)render::render_page(doc.page(0), dpi, options, &doc);
    } catch (const genko::core::Error& e) {
        return e.code() + ": " + e.what();
    }
    return "(drawn)";
}

}  // namespace

class TestPlaced : public QObject {
    Q_OBJECT

private slots:
    void pictureSizeReadsTheHeaderOnly() {
        for (const Sample& s : kSamples) {
            const std::string whole = genko::core::a2b_base64(s.whole);
            const std::string cut = genko::core::a2b_base64(s.cut);
            QCOMPARE(render::picture_size(whole), render::open_image(whole, render::kPillowOpenLimits).size());
            QCOMPARE(render::picture_size(whole), (render::Size{9, 6}));
            QCOMPARE(render::picture_size(cut), (render::Size{9, 6}));  // (as Image.open: the header is whole)
            QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::open_image(cut, render::kPillowOpenLimits));  // (as load())
        }
        // (no header to read: Python's errors, as before)
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::picture_size(std::string("\xff\xd8\xff\xe0", 4)));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::picture_size(std::string("BM", 2)));
        QVERIFY_THROWS_EXCEPTION(genko::core::Error, render::picture_size(std::string("GIF89a", 6)));
    }

    void aBrokenPictureIsNotPlaced() {
        for (const Sample& s : kSamples) {
            QTemporaryDir tmp;
            const Document doc = book_in(tmp.path());
            const std::string ref = put(tmp.path(), genko::core::a2b_base64(s.cut));
            const std::string leaf = doc.page(0).frames[0].id;
            const Json note = Json::object({{"op", "set_note"}, {"page", 1}, {"note", "前置き"}});
            const auto candidate = [&leaf](const std::string& asset) {
                return Json::object({{"op", "import_candidates"}, {"page", 1}, {"frame_id", leaf},
                                     {"candidates", Json::array({Json::object({{"id", "cd_1"}, {"asset", asset}, {"px", Json::array({9, 6})},
                                                                               {"origin", Json::object({{"kind", "agent"}})}})})}});
            };
            const Json adopt = Json::object({{"op", "adopt_candidate"}, {"page", 1}, {"frame_id", leaf}, {"candidate_id", "cd_1"}});
            // the batch refused at the op that places it, after the changes before it (nothing of them kept: the book is
            // as it was), its size read from the header as Python reads it
            const std::string cannot = "apply: ops[1] ";
            const std::string placed = refused(doc, Json::array({note, Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"frame_id", leaf}})}));
            QVERIFY2(placed.starts_with(cannot + "place_asset: the asset's picture cannot be read ("), (s.name + (": " + placed)).c_str());
            const std::string adopted = refused(doc, Json::array({candidate(ref), adopt}));
            QVERIFY2(adopted.starts_with(cannot + "adopt_candidate: the asset's picture cannot be read ("), (s.name + (": " + adopted)).c_str());
            const std::string moved = refused(with_placed(doc, ref), Json::array({note, Json::object({{"op", "set_placement"}, {"page", 1}, {"frame_id", leaf}, {"scale", 0.5}})}));
            QVERIFY2(moved.starts_with(cannot + "set_placement: the asset's picture cannot be read ("), (s.name + (": " + moved)).c_str());
            // the whole picture placed each way
            const std::string good = put(tmp.path(), genko::core::a2b_base64(s.whole));
            const auto done = bus().apply(doc,
                                          Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", good}, {"to", "bg"}, {"frame_id", leaf}}),
                                                       candidate(good), adopt}),
                                          Actor("human:作者"));
            QCOMPARE(done.applied.size(), std::size_t{3});
            const auto again = bus().apply(with_placed(doc, good),
                                           Json::array({Json::object({{"op", "set_placement"}, {"page", 1}, {"frame_id", leaf}, {"scale", 0.5}})}),
                                           Actor("human:作者"));
            QCOMPARE(again.applied.size(), std::size_t{1});
        }
        // (bytes that are no picture, which Python's adopt_candidate takes without looking, refused the same; a
        // candidate's picture this build does not open yet, and one no longer in the book's assets, adopted as Python
        // adopts them — the drawing says what it cannot draw)
        QTemporaryDir tmp;
        const Document doc = book_in(tmp.path());
        const std::string leaf = doc.page(0).frames[0].id;
        const auto adopting = [&leaf](const std::string& ref) {
            return Json::array({Json::object({{"op", "import_candidates"}, {"page", 1}, {"frame_id", leaf},
                                              {"candidates", Json::array({Json::object({{"id", "cd_1"}, {"asset", ref}, {"px", Json::array({9, 6})},
                                                                                        {"origin", Json::object({{"kind", "agent"}})}})})}}),
                                Json::object({{"op", "adopt_candidate"}, {"page", 1}, {"frame_id", leaf}, {"candidate_id", "cd_1"}})});
        };
        const std::string said = refused(doc, adopting(put(tmp.path(), "this is not a picture")));
        QVERIFY2(said.starts_with("apply: ops[1] adopt_candidate: the asset's picture cannot be read ("), said.c_str());
        const std::string webp = put(tmp.path(), genko::core::a2b_base64(kWebp));
        QCOMPARE(QString::fromStdString(refused(doc, adopting(webp))), QStringLiteral("(applied)"));
        const std::string gone = put(tmp.path(), genko::core::a2b_base64(kSamples[0].cut));
        const auto imported = bus().apply(doc, Json::array({adopting(gone)[0]}), Actor("human:作者"));  // (import_candidates looks for it)
        QVERIFY(fs::remove(genko::storage::AssetStore(*doc.asset_dir).path(gone, ".png")));
        QCOMPARE(QString::fromStdString(refused(imported.doc, Json::array({adopting(gone)[1]}))), QStringLiteral("(applied)"));
    }

    void aBrokenPictureFromElsewhereStopsItsDrawingAndExport() {
        // (a book written elsewhere — by Python, which places such a picture —: nothing placed it here)
        for (const Sample& s : kSamples) {
            QTemporaryDir tmp;
            const Document doc = with_placed(book_in(tmp.path()), put(tmp.path(), genko::core::a2b_base64(s.cut)));
            for (const char* mode : {"print", "proof", "name"}) {
                const std::string said = drawing_says(doc, 60, mode);
                QVERIFY2(said.starts_with("format: "), (std::string(s.name) + " " + mode + ": " + said).c_str());
            }
            // the export says so, and writes nothing
            const fs::path out = genko::core::path_from_utf8((tmp.path() + "/out").toStdString());
            genko::formats::RunOptions at40;
            at40.dpi = 40;
            const Json reply = genko::formats::run(doc, std::nullopt, "png", out, at40);
            QCOMPARE(reply.value("ok", true), false);
            QVERIFY2(reply.value("error", std::string()).find("JPEG") != std::string::npos ||
                         reply.value("error", std::string()).find("truncated") != std::string::npos ||
                         reply.value("error", std::string()).find("broken") != std::string::npos,
                     (s.name + (": " + genko::core::dump_python(reply))).c_str());
            QVERIFY(!fs::exists(out) || fs::is_empty(out));
        }
    }

    void anAssetPastWhatTheStoreReadsIsRefused() {
        // (a file of 256 MiB and a byte, sparse: no disk taken)
        QTemporaryDir tmp;
        Document doc = book_in(tmp.path());
        const std::string ref = "sha256:" + std::string(64, 'a');
        const fs::path file = genko::storage::AssetStore(*doc.asset_dir).path(ref, ".png");
        fs::create_directories(file.parent_path());
        {
            std::ofstream out(file, std::ios::binary);
            out << "\x89PNG";
        }
        fs::resize_file(file, genko::storage::kAssetMaxBytes + 1);
        std::string said;
        try {
            (void)bus().apply(doc, Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", ref}})}), Actor("human:作者"));
        } catch (const ApplyError& e) {
            said = e.code() + ": " + e.what();
        }
        QVERIFY2(said.starts_with("apply: ops[0] place_asset: the asset's picture cannot be read ("), said.c_str());
    }

    void aDigestNamingAnotherPlaceIsNoAsset() {
        // (C++ only: a digest with ":" — a drive on Windows —, "/", "\" or a leading "." is no asset, even where a file of
        // that name is there; Python's glob would find it)
        QTemporaryDir tmp;
        Document doc = book_in(tmp.path());
        const QString folder = tmp.path() + "/assets/ab";
        QVERIFY(QDir().mkpath(folder));
        {
            std::ofstream out(genko::core::path_from_utf8((folder + "/ab:cd.png").toStdString()), std::ios::binary);
            out << genko::core::a2b_base64(kSamples[1].whole);
        }
        std::string said;
        try {
            (void)bus().apply(doc, Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", "sha256:ab:cd"}})}), Actor("human:作者"));
        } catch (const ApplyError& e) {
            said = e.code() + ": " + e.what();
        }
        QVERIFY2(said.starts_with("apply: ops[0] place_asset: asset sha256:ab:cd is not in assets/"), said.c_str());
    }

    void aWebpIsNotOpenedYet() {
        QTemporaryDir tmp;
        Document doc = book_in(tmp.path());
        const std::string ref = put(tmp.path(), genko::core::a2b_base64(kWebp));
        std::string said;
        try {
            (void)bus().apply(doc, Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", ref}})}), Actor("human:作者"));
        } catch (const ApplyError& e) {
            said = e.code() + ": " + e.what();
        }
        QVERIFY2(said.starts_with("not_yet_ported: ops[0] place_asset: "), said.c_str());
    }

    void aScreenTooCoarseIsRefused() {
        // (a finish at 1 line an inch: a 106 × 106 px cell at 150 dpi, a threshold tile 22472 px a side — Python builds
        // it pixel by pixel, for hours; this build refuses past 4096 px)
        QTemporaryDir tmp;
        Document doc = book_in(tmp.path());
        const std::string ref = put(tmp.path(), genko::core::a2b_base64(kSamples[1].whole));
        const std::string leaf = doc.page(0).frames[0].id;
        const auto done = bus().apply(doc,
                                      Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"frame_id", leaf}}),
                                                   Json::object({{"op", "set_finish"}, {"page", 1}, {"frame_id", leaf}, {"finish", Json::object({{"lpi", 1}})}})}),
                                      Actor("human:作者"));
        QCOMPARE(QString::fromStdString(drawing_says(done.doc, 150, "print")),
                 QStringLiteral("value: the screen is too coarse for this resolution (22472 px a side)"));
        QCOMPARE(QString::fromStdString(drawing_says(done.doc, 150, "proof")), QStringLiteral("(drawn)"));  // (no dots in a proof)
        QCOMPARE(QString::fromStdString(drawing_says(done.doc, 30, "print")), QStringLiteral("(drawn)"));  // (a cell of 30 px)
    }

    void picturesRememberedAreLetGo() {
        QTemporaryDir tmp;
        Document doc = book_in(tmp.path());
        const std::string ref = put(tmp.path(), genko::core::a2b_base64(kSamples[1].whole));
        const auto done = bus().apply(doc, Json::array({Json::object({{"op", "place_asset"}, {"page", 1}, {"asset", ref}, {"frame_id", doc.page(0).frames[0].id}})}),
                                      Actor("human:作者"));
        render::clear_placed_caches();
        QCOMPARE(render::placed_cache_pixels(), std::int64_t{0});
        render::RenderOptions options;
        options.region = render::RenderRegion{0, 0, 100, 100};  // (a tile: the finished art is kept for the next ones)
        (void)render::render_page(done.doc.page(0), 60, options, &done.doc);
        QVERIFY(render::placed_cache_pixels() > 0);
        render::clear_placed_caches();
        QCOMPARE(render::placed_cache_pixels(), std::int64_t{0});
    }
};

QTEST_GUILESS_MAIN(TestPlaced)
#include "test_placed.moc"
