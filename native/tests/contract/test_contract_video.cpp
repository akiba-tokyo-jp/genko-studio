// PC5 J-e2 (video@1, SPEC.md PROD-01「ビデオレイヤー」) as a person uses it:
//  - through the command line: footage read into a book (render/footage: a GIF's frames, the book's assets), a video layer
//    added with `genko apply` (the page made an animation at the footage's speed, the book naming video@1), `genko inspect
//    --timeline` saying its footage and frames, `genko render --frame` drawing the footage's frame, a blank frame inserted
//    and undone with `genko undo` (the pages as before — their content: the journal's state gives an anim's keys in their
//    names' order, as it does any page's extra);
//  - the ops on a book in memory: frames inserted, duplicated (an altered frame's cel copied) and deleted (its cel gone),
//    the timeline made longer; a frame altered (its cel holding the footage's frame) shown or not (変更されたビデオを表示),
//    restored, all restored; the footage interpreted at another speed (its frames laid again, the altered ones where they
//    were) and replaced (frames past its end blank); rasterized at a frame (the video layer gone, a paint layer of what it
//    showed in its place); ビデオフレームからレイヤー into layers and into an animation folder's cels;
//  - refused: not a video layer, a frame outside or not a whole number, a locked video layer (locked, すべてをロック), another's
//    page lock, footage the book does not hold, the book's footage past 4 GiB, more than 500 frames to layers; not an
//    agent's op (MCP); strict_gates before the name (the footage's pixels made printed layers, printed layers taken away);
//    a hand-written timeline of 100 million frames never laid frame by frame; the footage interpreted keeping its edits;
//  - written: video@1 only while a page has a video row, its footage frames among the book's assets, read back the same
//    and written again byte for byte; a footage frame gone from the book read-only; a book without video rows (an
//    animation of cels) drawn as before — frame_page is at_frame — and named as before;
//  - written out: ビデオをレンダリング (a GIF of the frames the footage shows), a .pcraft (the first frame's footage as a layer,
//    said; the timeline written and read back; with $GENKO_PCRAFT_CHECK, upstream's reader reads it).

#include <QtTest>

#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/json.hpp"
#include "core/video.hpp"
#include "formats/open_file.hpp"
#include "formats/pcraft_write.hpp"
#include "render/anim.hpp"
#include "render/codecs/codecs.hpp"
#include "render/footage.hpp"
#include "render/movie.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "service/studio_service.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using namespace genko;
using core::Json;
namespace fs = std::filesystem;

namespace {

core::Document run(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    return core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor(actor)).doc;
}

Json reported(const core::Document& doc, const Json& op) {
    const auto done = core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor("human:test"));
    return done.results.empty() ? Json() : done.results[0];
}

std::string refusal(const core::Document& doc, const Json& op, const char* actor = "human:test") {
    try {
        (void)core::CommandBus(render::ops_registry()).apply(doc, Json::array({op}), core::Actor(actor));
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

Json op(const char* name, Json more = Json::object()) {
    Json out{{"op", name}, {"page", 1}};
    for (auto& [k, v] : more.items()) out[k] = v;
    return out;
}

// A 96 × 72 mm colour page at 200 dpi, without panels.
core::Document book() {
    auto doc = core::new_episode("ビデオ", core::Num(1), 1, core::PageSpec::custom(96, 72, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
    auto& p = doc.edit_page(0);
    p.numero = false;
    p.frames.clear();
    return doc;
}

const int kColours[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};

render::Image solid(int i) {
    return render::Image::create("RGBA", render::Size{32, 24}, render::Ink{kColours[i][0], kColours[i][1], kColours[i][2], 255}).convert("RGB");
}

// The page's middle as it shows at that frame (50 dpi).
std::vector<double> middle(const core::Document& doc, std::int64_t frame) {
    render::RenderOptions options;
    options.mode = "proof";
    options.at_frame = true;
    const render::Image drawn = render::render_page(render::footage::frame_page(doc.page(0), frame, &doc), 50, options, &doc).image.convert("RGB");
    return drawn.getpixel(drawn.width() / 2, drawn.height() / 2);
}

// A point of a page (mm) as it shows at that frame (50 dpi).
std::vector<double> at_mm(const core::Document& doc, std::size_t page, std::int64_t frame, double x_mm, double y_mm) {
    render::RenderOptions options;
    options.mode = "proof";
    options.at_frame = true;
    const render::Image drawn = render::render_page(render::footage::frame_page(doc.page(page), frame, &doc), 50, options, &doc).image.convert("RGB");
    return drawn.getpixel(static_cast<int>(x_mm / 25.4 * 50), static_cast<int>(y_mm / 25.4 * 50));
}

bool near(const std::vector<double>& px, const int rgb[3], int tolerance = 8) {
    for (int c = 0; c < 3; ++c)
        if (std::fabs(px[c] - rgb[c]) > tolerance) return false;
    return true;
}

const int kWhite[3] = {255, 255, 255};

// Where two values first differ ("": nowhere).
std::string first_difference(const Json& a, const Json& b, const std::string& at = "") {
    if (a.type() != b.type()) return at + ": " + a.dump().substr(0, 200) + " | " + b.dump().substr(0, 200);
    if (a.is_object()) {
        for (const auto& [key, value] : a.items()) {
            if (!b.contains(key)) return at + "/" + key + ": only before";
            if (std::string d = first_difference(value, b.at(key), at + "/" + key); !d.empty()) return d;
        }
        for (const auto& [key, value] : b.items())
            if (!a.contains(key)) return at + "/" + key + ": only after";
        return {};
    }
    if (a.is_array()) {
        if (a.size() != b.size()) return at + ": " + std::to_string(a.size()) + " | " + std::to_string(b.size()) + " items";
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::string d = first_difference(a[i], b[i], at + "/" + std::to_string(i)); !d.empty()) return d;
        return {};
    }
    return a == b ? std::string() : at + ": " + a.dump().substr(0, 200) + " | " + b.dump().substr(0, 200);
}

}  // namespace

class TestContractVideo : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString path(const QString& rel) const { return tmp_.path() + QStringLiteral("/") + rel; }
    fs::path fspath(const QString& rel) const { return fs::path(path(rel).toStdString()); }
    Json genko(const QStringList& args, int* exit = nullptr) {
        const auto ran = genko::test::run_genko(args);
        if (exit != nullptr) *exit = ran.exit_code;
        if (!ran.finished) return Json{{"not_run", ran.described().toStdString()}};
        return genko::test::one_line(ran.out);
    }
    Json apply(const QString& book, const QString& name, const Json& ops) {
        genko::test::write_bytes(path(name), ops.dump());
        int exit = -1;
        const Json out = genko({QStringLiteral("apply"), book, path(name), QStringLiteral("--agent"), QStringLiteral("human:test")}, &exit);
        if (exit != 0) return Json{{"failed", out}};
        return out;
    }
    // A GIF of the four colours, 10 fps.
    fs::path four() {
        const fs::path out = fspath(QStringLiteral("four.gif"));
        if (!fs::exists(out)) (void)render::movie::write_movie({solid(0), solid(1), solid(2), solid(3)}, out, 10, "gif");
        return out;
    }
    // The four colours' footage held by the book in memory.
    Json footage(core::Document& doc) { return render::footage::read_footage(four(), {}, render::footage::memory_store(doc)).json; }
    // A book of a video layer "v" of the four colours.
    core::Document video_book() {
        core::Document doc = book();
        const Json f = footage(doc);
        return run(doc, op("add_video_layer", {{"footage", f}, {"id", "v"}}));
    }
    static const Json& frames_of(const core::Document& doc) { return core::video_row(doc.page(0), Json("v"))->at("frames"); }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void throughTheCommandLine() {
        const QString book_path = path(QStringLiteral("book.genko"));
        genko::test::write_project(book(), fs::path(book_path.toStdString()));
        const Json f = render::footage::read_footage(four(), {}, render::footage::book_store(fs::path(book_path.toStdString()))).json;
        QCOMPARE(f["frames"].size(), std::size_t(4));
        for (const Json& ref : f["frames"]) QVERIFY(storage::AssetStore(fs::path(book_path.toStdString())).has(ref.get<std::string>(), ".png"));
        Json done = apply(book_path, QStringLiteral("a.json"), Json::array({op("add_video_layer", {{"footage", f}, {"id", "v"}, {"name", "四色"}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        const Json project = genko::test::read_json(book_path + QStringLiteral("/project.json"));
        const Json features = project["features"];
        QVERIFY2(std::find(features.begin(), features.end(), Json("video@1")) != features.end(), features.dump().c_str());
        QVERIFY(std::find(features.begin(), features.end(), Json("timeline@1")) == features.end());  // (10 fps)
        const Json anim = project["pages"][0]["anim"];
        QCOMPARE(anim["frames"], Json(4));
        QCOMPARE(anim["fps"], Json(10.0));
        // inspect --timeline
        int exit = -1;
        const Json info = genko({QStringLiteral("inspect"), book_path, QStringLiteral("--timeline"), QStringLiteral("--page"), QStringLiteral("1")}, &exit);
        QCOMPARE(exit, 0);
        const Json track = info["pages"][0]["tracks"][0];
        QCOMPARE(track["name"], Json("四色"));
        QCOMPARE(track["video"]["footage"]["frames"], Json(4));
        QCOMPARE(track["video"]["footage"]["kind"], Json("animation"));
        QCOMPARE(track["video"]["frames"], Json(4));
        QCOMPARE(track["video"]["altered"], Json(0));
        // render --frame: the footage's frame
        for (int frame = 1; frame <= 4; ++frame) {
            const QString out = path(QStringLiteral("frame%1.png").arg(frame));
            const auto drawn = genko::test::run_genko({"render", book_path, "--page", "1", "--dpi", "50", "--frame", QString::number(frame), "--out", out});
            QVERIFY2(drawn.finished && drawn.exit_code == 0, (drawn.out + drawn.err).constData());
            const render::Image image = render::read_png(genko::test::read_bytes(out)).convert("RGB");
            QVERIFY2(near(image.getpixel(image.width() / 2, image.height() / 2), kColours[frame - 1]), std::to_string(frame).c_str());
        }
        // a blank frame inserted, undone
        const Json before = genko::test::read_json(book_path + QStringLiteral("/project.json"));
        done = apply(book_path, QStringLiteral("b.json"), Json::array({op("video_insert_blank", {{"folder", "v"}, {"at", 2}})}));
        QVERIFY2(done.value("ok", false), done.dump().c_str());
        QCOMPARE(genko::test::read_json(book_path + QStringLiteral("/project.json"))["pages"][0]["anim"]["frames"], Json(5));
        (void)genko({QStringLiteral("undo"), book_path, QStringLiteral("--as"), QStringLiteral("human:test")}, &exit);
        QCOMPARE(exit, 0);
        const Json undone = genko::test::read_json(book_path + QStringLiteral("/project.json"));
        // (the same content — the journal's undo may give an object's keys in another order)
        const std::string differ = first_difference(before["pages"], undone["pages"]);
        QVERIFY2(differ.empty(), differ.c_str());
        QCOMPARE(undone["features"], before["features"]);
        // a footage frame gone from the book: read-only
        const QString copy = path(QStringLiteral("gone.genko"));
        genko::test::copy_tree(book_path, copy);
        fs::remove(storage::AssetStore(fs::path(copy.toStdString())).path(f["frames"][0].get<std::string>(), ".png"));
        const auto back = storage::load_document(fs::path(copy.toStdString()));
        QVERIFY2(back.document.read_only_reason.find("footage frame") != std::string::npos, back.document.read_only_reason.c_str());
    }

    void framesInsertedDuplicatedDeleted() {
        core::Document doc = video_book();
        QCOMPARE(frames_of(doc), Json::parse("[[0,null],[1,null],[2,null],[3,null]]"));
        QCOMPARE(core::anim::frames_of(doc.page(0)), std::int64_t(4));
        QCOMPARE(core::anim::fps_of(doc.page(0)), 10.0);
        doc = run(doc, op("video_insert_blank", {{"folder", "v"}, {"at", 2}}));
        QCOMPARE(frames_of(doc), Json::parse("[[0,null],[null,null],[1,null],[2,null],[3,null]]"));
        QCOMPARE(core::anim::frames_of(doc.page(0)), std::int64_t(5));
        QVERIFY(near(middle(doc, 2), kWhite));
        QVERIFY(near(middle(doc, 3), kColours[1]));
        // a frame altered: its cel holds the footage's frame
        const Json altered = reported(doc, op("video_alter", {{"folder", "v"}, {"at", 3}}));
        QCOMPARE(altered["made"], Json(true));
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 3}}));
        const std::string cel = frames_of(doc)[2][1].get<std::string>();
        const auto cels = core::anim::cels_of(doc.page(0), Json("v"));
        QCOMPARE(cels.size(), std::size_t(1));
        QCOMPARE(cels[0]->id, cel);
        QVERIFY(cels[0]->raster_png);
        QVERIFY(near(middle(doc, 3), kColours[1]));
        QCOMPARE(reported(doc, op("video_alter", {{"folder", "v"}, {"at", 3}}))["made"], Json(false));  // (it has one)
        // duplicated: the cel copied
        doc = run(doc, op("video_duplicate", {{"folder", "v"}, {"at", 3}}));
        QCOMPARE(frames_of(doc).size(), std::size_t(6));
        QVERIFY(frames_of(doc)[3][1].is_string() && frames_of(doc)[3][1] != frames_of(doc)[2][1]);
        QCOMPARE(core::anim::cels_of(doc.page(0), Json("v")).size(), std::size_t(2));
        QCOMPARE(core::anim::frames_of(doc.page(0)), std::int64_t(6));
        // deleted: its cel gone, those after it one earlier
        const std::string copied = frames_of(doc)[3][1].get<std::string>();
        doc = run(doc, op("video_delete", {{"folder", "v"}, {"at", 4}}));
        QCOMPARE(frames_of(doc).size(), std::size_t(5));
        QCOMPARE(core::anim::cels_of(doc.page(0), Json("v")).size(), std::size_t(1));
        QVERIFY(std::none_of(doc.page(0).layers.begin(), doc.page(0).layers.end(), [&](const core::Layer& l) { return l.id == copied; }));
        QCOMPARE(core::anim::frames_of(doc.page(0)), std::int64_t(6));  // (the timeline as long as it was)
        // 変更されたビデオを表示 off: the footage shows (the cel hidden at its frame)
        doc = run(doc, op("set_video_layer", {{"folder", "v"}, {"show_altered", false}}));
        const core::Page shown = render::footage::frame_page(doc.page(0), 3, &doc);
        for (const core::Layer& l : shown.layers)
            if (l.id == cel) QVERIFY(!l.visible);
        doc = run(doc, op("set_video_layer", {{"folder", "v"}, {"show_altered", true}}));
        // restored: the cel gone, the footage again
        doc = run(doc, op("video_restore", {{"folder", "v"}, {"at", 3}}));
        QVERIFY(frames_of(doc)[2][1].is_null());
        QVERIFY(core::anim::cels_of(doc.page(0), Json("v")).empty());
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 1}}));
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 5}}));
        QCOMPARE(reported(doc, op("video_restore_all", {{"folder", "v"}}))["restored"], Json(2));
        doc = run(doc, op("video_restore_all", {{"folder", "v"}}));
        QVERIFY(core::anim::cels_of(doc.page(0), Json("v")).empty());
        // a cel deleted as a layer: its frame shows the footage again
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 1}}));
        doc = run(doc, op("delete_layers", {{"ids", Json::array({frames_of(doc)[0][1]})}}));
        QVERIFY(frames_of(doc)[0][1].is_null());
    }

    // PC6 review B P2-3: a page copied (duplicate_page) or brought from another book (import_pages) — its layers under new
    // ids —: the video row's altered frame names the copy's cel, which shows there (before: the footage showed, the cel
    // hidden in the folder).
    void alteredFramesFollowTheirCopies() {
        core::Document doc = run(video_book(), op("video_alter", {{"folder", "v"}, {"at", 2}}));
        const std::string cel = frames_of(doc)[1][1].get<std::string>();
        const auto altered_on = [](const core::Document& d, std::size_t page) {
            const Json& track = core::anim::spec(d.page(page))->at("tracks").at(0);
            const core::VideoShown shown = core::video_shown(d.page(page), track.at("folder"), track.at("video"), 2);
            const bool held = std::any_of(d.page(page).layers.begin(), d.page(page).layers.end(), [&](const core::Layer& l) {
                return l.id == shown.cel && l.parent_id == track.at("folder");
            });
            return std::make_pair(shown.kind == core::VideoShown::Kind::Cel && held, shown.cel);
        };
        const core::Document twice = run(doc, Json{{"op", "duplicate_page"}, {"page", 1}});
        QCOMPARE(twice.pages.size(), std::size_t(2));
        const auto copied = altered_on(twice, 1);
        QVERIFY2(copied.first, copied.second.c_str());
        QVERIFY(copied.second != cel);
        QVERIFY(altered_on(twice, 0) == std::make_pair(true, cel));
        // (作品の結合: the other book's page under new ids)
        const fs::path dir = fspath(QStringLiteral("altered.genko"));
        genko::test::write_project(doc, dir);
        const core::Document merged = run(book(), Json{{"op", "import_pages"}, {"from", dir.string()}});
        QCOMPARE(merged.pages.size(), std::size_t(2));
        const auto brought = altered_on(merged, 1);
        QVERIFY2(brought.first, brought.second.c_str());
    }

    // PC6 review B P2-4: an artboard made of a video layer (artboard_from_layers) takes its row — the page's timeline with
    // it, the altered cel named under its new id —, the page it came from losing the row with the folder; a smart object is
    // not made of an animation folder or its cels (refused, in the window's words).
    void videoLayersMovedOrRefused() {
        core::Document doc = run(video_book(), op("video_alter", {{"folder", "v"}, {"at", 2}}));
        const std::string cel = frames_of(doc)[1][1].get<std::string>();
        const core::ApplyResult made = core::CommandBus(render::ops_registry()).apply(doc, Json::array({op("artboard_from_layers", {{"ids", Json::array({"v"})}})}),
                                                                                    core::Actor("human:test"));
        const core::Document& boards = made.doc;
        QCOMPARE(boards.pages.size(), std::size_t(2));
        const Json* anim = core::anim::spec(boards.page(1));
        QVERIFY(anim != nullptr);
        QCOMPARE(anim->at("tracks").size(), std::size_t(1));
        const Json& track = anim->at("tracks")[0];
        QVERIFY(core::is_video_row(track));
        QCOMPARE(core::anim::frames_of(boards.page(1)), core::anim::frames_of(doc.page(0)));
        QCOMPARE(core::anim::fps_of(boards.page(1)), core::anim::fps_of(doc.page(0)));
        const std::string moved_cel = track["video"]["frames"][1][1].get<std::string>();
        QVERIFY(moved_cel != cel);
        QVERIFY(std::any_of(boards.page(1).layers.begin(), boards.page(1).layers.end(), [&](const core::Layer& l) { return l.id == moved_cel; }));
        QCOMPARE(track["video"]["footage"], core::video_row(doc.page(0), Json("v"))->at("footage"));
        QVERIFY(core::anim::spec(boards.page(0))->at("tracks").empty());
        // (its footage box moved with the crop that made the artboard: the footage frame where the cel was)
        const Json& box = track["video"]["placement_mm"];
        const double cx = box[0].get<double>() + box[2].get<double>() / 2, cy = box[1].get<double>() + box[3].get<double>() / 2;
        QVERIFY(near(at_mm(boards, 1, 3, cx, cy), kColours[2]));
        QVERIFY(near(at_mm(boards, 1, 2, cx, cy), kColours[1]));
        // (a smart object: refused, the folder and its cel alike)
        for (const Json& ids : {Json::array({"v"}), Json::array({cel})}) {
            const std::string refused = refusal(doc, op("smart_convert", {{"ids", ids}}));
            QVERIFY2(refused.find("an animation folder or its cels (a video layer's too) cannot go into a smart object") != std::string::npos, refused.c_str());
            QVERIFY(wording::error(QString::fromStdString(refused)).contains(QStringLiteral("スマートオブジェクトにできません")));
        }
    }

    // PC6 review B P2-5: the page size ops move a video layer's footage box with everything else (the crop's offset; the
    // image size's scale), so the footage and its altered cels stay together; a turn or a mirror of the page is refused (the
    // footage is laid upright in its box), in the window's words.
    void footageBoxMovesWithThePage() {
        core::Document doc = run(video_book(), op("video_alter", {{"folder", "v"}, {"at", 2}}));
        const Json was = core::video_row(doc.page(0), Json("v"))->at("placement_mm");
        const core::ApplyResult cropped = core::CommandBus(render::ops_registry()).apply(doc, Json::array({op("crop_page", {{"rect", Json::array({30, 20, 50, 40})}})}),
                                                                                       core::Actor("human:test"));
        const Json& offset = cropped.results[0]["pages"][0]["offset_mm"];
        const Json now = core::video_row(cropped.doc.page(0), Json("v"))->at("placement_mm");
        for (int k = 0; k < 2; ++k) QVERIFY2(std::fabs(now[k].get<double>() - (was[k].get<double>() + offset[k].get<double>())) < 1e-3, now.dump().c_str());
        QCOMPARE(now[2], was[2]);
        QCOMPARE(now[3], was[3]);
        const double cx = now[0].get<double>() + now[2].get<double>() / 2, cy = now[1].get<double>() + now[3].get<double>() / 2;
        QVERIFY(near(at_mm(cropped.doc, 0, 3, cx, cy), kColours[2]));  // (the footage)
        QVERIFY(near(at_mm(cropped.doc, 0, 2, cx, cy), kColours[1]));  // (the altered cel, the pixels moved)
        const core::Document hidden = run(cropped.doc, op("set_video_layer", {{"folder", "v"}, {"show_altered", false}}));
        QVERIFY(near(at_mm(hidden, 0, 2, cx, cy), kColours[1]));  // (the footage where the cel is)
        // (画像解像度: twice as large, the box with it)
        const core::Document larger = run(doc, op("resize_pages", {{"width_mm", 192}, {"height_mm", 144}}));
        const Json big = core::video_row(larger.page(0), Json("v"))->at("placement_mm");
        for (int k = 0; k < 4; ++k) QVERIFY2(std::fabs(big[k].get<double>() - 2 * was[k].get<double>()) < 1e-3, big.dump().c_str());
        // (turned or mirrored: refused, the book as it was)
        for (const char* turn : {"90cw", "180", "flip_h"}) {
            const std::string refused = refusal(doc, op("rotate_pages", {{"turn", turn}}));
            QVERIFY2(refused.find("a video layer turned or mirrored with the page") != std::string::npos, refused.c_str());
            QVERIFY(wording::error(QString::fromStdString(refused)).contains(QStringLiteral("ビデオレイヤーのあるページは回転・反転できません")));
        }
        // (a page without a video row turns as before)
        QCOMPARE(refusal(book(), op("rotate_pages", {{"turn", "90cw"}})), std::string());
    }

    void footageInterpretedReplacedRasterized() {
        core::Document doc = video_book();
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 2}}));
        // at 5 fps on a 10 fps timeline: each frame shown twice, the altered frame where it was
        doc = run(doc, op("set_video_layer", {{"folder", "v"}, {"fps", 5}, {"alpha", "ignore"}, {"par", 2}, {"fit", "stretch"}}));
        const Json& frames = frames_of(doc);
        QCOMPARE(frames.size(), std::size_t(8));
        for (std::size_t k = 0; k < 8; ++k) QCOMPARE(frames[k][0], Json(k / 2));
        QVERIFY(frames[1][1].is_string());
        QCOMPARE(core::anim::frames_of(doc.page(0)), std::int64_t(8));
        const Json& f = core::video_row(doc.page(0), Json("v"))->at("footage");
        QCOMPARE(f["alpha"], Json("ignore"));
        QCOMPARE(f["par"], Json(2.0));
        QVERIFY(near(middle(doc, 5), kColours[2]));
        QVERIFY(!refusal(doc, op("set_video_layer", {{"folder", "v"}, {"fps", 0.1}})).empty());
        QVERIFY(!refusal(doc, op("set_video_layer", {{"folder", "v"}, {"speed", 2}})).empty());
        // replaced by two pictures: frames past them blank
        render::footage::ReadOptions two;
        two.range = std::array<std::int64_t, 2>{3, 4};
        const Json other = render::footage::read_footage(four(), two, render::footage::memory_store(doc)).json;
        doc = run(doc, op("replace_footage", {{"folder", "v"}, {"footage", other}}));
        QCOMPARE(frames_of(doc)[0][0], Json(0));
        QCOMPARE(frames_of(doc)[3][0], Json(1));
        QVERIFY(frames_of(doc)[4][0].is_null());
        QVERIFY(near(middle(doc, 1), kColours[2]));
        QVERIFY(near(middle(doc, 7), kWhite));
        // rasterized at frame 4: a paint layer of what it showed in its place, the video layer gone
        const std::size_t layers = doc.page(0).layers.size();
        const Json raster = reported(doc, op("rasterize_video", {{"folder", "v"}, {"at", 4}}));
        doc = run(doc, op("rasterize_video", {{"folder", "v"}, {"at", 4}, {"id", raster["id"]}}));
        QVERIFY(core::video_row(doc.page(0), Json("v")) == nullptr);
        QVERIFY(!core::page_uses_video(doc.page(0)));
        QCOMPARE(doc.page(0).layers.size(), layers - 1);  // (the folder and its cel: one paint layer)
        const auto it = std::find_if(doc.page(0).layers.begin(), doc.page(0).layers.end(), [&](const core::Layer& l) { return Json(l.id) == raster["id"]; });
        QVERIFY(it != doc.page(0).layers.end() && it->kind == core::LayerKind::Raster && it->raster_png);
        QVERIFY(near(middle(doc, 1), kColours[3]));  // (on every frame now)
    }

    void framesToLayers() {
        core::Document doc = book();
        const Json f = footage(doc);
        const Json layers = reported(doc, op("footage_to_layers", {{"footage", f}, {"every", 2}}));
        QCOMPARE(layers["ids"].size(), std::size_t(2));
        doc = run(doc, op("footage_to_layers", {{"footage", f}}));
        QCOMPARE(doc.page(0).layers.size(), book().page(0).layers.size() + 4);
        QVERIFY(!core::anim::is_animation(doc.page(0)));
        QCOMPARE(doc.page(0).layers.back().title, std::string("フレーム 4"));
        core::Document celled = book();
        const Json g = footage(celled);
        celled = run(celled, op("footage_to_layers", {{"footage", g}, {"into", "cels"}, {"range", Json::array({2, 4})}}));
        QVERIFY(core::anim::is_animation(celled.page(0)));
        const Json folder = core::anim::folders(celled.page(0)).at(0);
        QCOMPARE(core::anim::cels_of(celled.page(0), folder).size(), std::size_t(3));
        QCOMPARE(core::anim::frames_of(celled.page(0)), std::int64_t(24));
        QVERIFY(near(middle(celled, 2), kColours[2]));
        // at most 500
        Json many = f;
        many["frames"] = Json::array();
        for (int i = 0; i < 501; ++i) many["frames"].push_back(f["frames"][i % 2]);
        QVERIFY2(refusal(doc, op("footage_to_layers", {{"footage", many}})).find("at most 500") != std::string::npos, refusal(doc, op("footage_to_layers", {{"footage", many}})).c_str());
    }

    void refused() {
        core::Document doc = video_book();
        doc = run(doc, op("add_anim_folder", {{"id", "plain"}}));
        QVERIFY(refusal(doc, op("video_insert_blank", {{"folder", "plain"}, {"at", 1}})).find("video layer") != std::string::npos);
        QVERIFY(refusal(doc, op("video_insert_blank", {{"folder", "v"}, {"at", 99}})).find("at is a frame") != std::string::npos);
        QVERIFY(refusal(doc, op("video_delete", {{"folder", "v"}})).find("at is a frame") != std::string::npos);
        core::Document locked = run(doc, op("set_layer", {{"id", "v"}, {"locked", true}}));
        QVERIFY(refusal(locked, op("video_delete", {{"folder", "v"}, {"at", 1}})).find("locked") != std::string::npos);
        core::Document all = run(doc, op("set_layer", {{"id", "v"}, {"lock_all", true}}));
        QVERIFY(refusal(all, op("video_alter", {{"folder", "v"}, {"at", 1}})).find("locked") != std::string::npos);
        QVERIFY(refusal(all, op("rasterize_video", {{"folder", "v"}, {"at", 1}})).find("locked") != std::string::npos);
        // another's page lock
        core::Document held = doc;
        held.page_locks[held.page(0).id] = "ai:other";
        QVERIFY(refusal(held, op("video_insert_blank", {{"folder", "v"}, {"at", 1}})).find("locked by ai:other") != std::string::npos);
        // footage the book does not hold; footage past 4 GiB
        Json f = core::video_row(doc.page(0), Json("v"))->at("footage");
        Json missing = f;
        missing["frames"] = Json::array({"sha256:" + std::string(64, 'a')});
        QVERIFY(refusal(doc, op("add_video_layer", {{"footage", missing}})).find("is not in the book") != std::string::npos);
        // (a footage that says another size than its frames are)
        Json wrong = f;
        wrong["size_px"] = Json::array({40000, 30000});
        QVERIFY2(refusal(doc, op("add_video_layer", {{"footage", wrong}})).find("size_px") != std::string::npos, refusal(doc, op("add_video_layer", {{"footage", wrong}})).c_str());
        // (the book's footage past 4 GiB: a row of 3000 pictures of 10000 × 10000 already there, one more refused)
        core::Document full = doc;
        Json big = f;
        big["size_px"] = Json::array({10000, 10000});
        big["frames"] = Json::array();
        for (int i = 0; i < 11; ++i) {
            char hex[65];
            std::snprintf(hex, sizeof(hex), "%064x", i + 1);
            big["frames"].push_back(std::string("sha256:") + hex);
        }
        (*full.edit_page(0).extra.find("anim"))["tracks"][0]["video"]["footage"] = big;
        QVERIFY(core::book_footage_bytes(full) > core::kFootageMaxBytes);
        const std::string past = refusal(full, op("add_video_layer", {{"footage", f}}));
        QVERIFY2(past.find("4 GiB") != std::string::npos, past.c_str());
        Json odd = f;
        odd["kind"] = "movie";
        QVERIFY(!refusal(doc, op("add_video_layer", {{"footage", odd}})).empty());
        // not an agent's op through MCP
        for (const char* name : {"add_video_layer", "video_insert_blank", "video_duplicate", "video_delete", "video_restore", "video_restore_all",
                                 "video_alter", "set_video_layer", "replace_footage", "rasterize_video", "footage_to_layers"}) {
            QVERIFY2(!service::agent_op(name), name);
        }
    }

    // (the coordinator's decision on the review's P2-5) strict_gates: what makes printed layers of the footage's pixels or
    // takes printed layers away needs the page's name approved; a blank video layer and a blank frame do not.
    void strictGates() {
        core::Document doc = book();
        const Json f = footage(doc);
        doc.strict_gates = true;
        const std::string first = refusal(doc, op("add_video_layer", {{"footage", f}, {"id", "v"}}));
        QVERIFY2(first.find("needs name_ok") != std::string::npos, first.c_str());
        QVERIFY(refusal(doc, op("footage_to_layers", {{"footage", f}})).find("needs name_ok") != std::string::npos);
        core::Document blank = run(doc, op("add_video_layer", {{"id", "b"}}));  // (nothing printed made)
        QVERIFY(core::video_row(blank.page(0), Json("b")) != nullptr);
        blank = run(blank, op("video_insert_blank", {{"folder", "b"}, {"at", 1}}));
        QVERIFY(refusal(blank, op("video_alter", {{"folder", "b"}, {"at", 1}})).find("needs name_ok") != std::string::npos);
        // (the name approved: made; then taken back to a page whose name is not approved — every op that takes printed
        // layers away refused)
        core::Document approved = doc;
        approved.edit_page(0).name_ok = true;
        approved = run(approved, op("add_video_layer", {{"footage", f}, {"id", "v"}}));
        approved = run(approved, op("video_alter", {{"folder", "v"}, {"at", 2}}));
        core::Document unapproved = approved;
        unapproved.edit_page(0).name_ok = false;
        for (const char* name : {"video_restore", "video_delete", "video_duplicate", "rasterize_video"}) {
            const std::string why = refusal(unapproved, op(name, {{"folder", "v"}, {"at", 2}}));
            QVERIFY2(why.find("needs name_ok") != std::string::npos, (std::string(name) + ": " + why).c_str());
        }
        QVERIFY(refusal(unapproved, op("video_restore_all", {{"folder", "v"}})).find("needs name_ok") != std::string::npos);
        QVERIFY(refusal(unapproved, op("video_insert_blank", {{"folder", "v"}, {"at", 2}})).empty());
    }

    // (the review's P2-2, P3-10, P3-11) A hand-written timeline of 100 million frames: a blank video layer of 3000 frames at
    // most, made at once, a frame past 3000 refused; `at` a whole number only; the footage interpreted at another speed
    // keeping the blank frame inserted and the frame deleted.
    void bigTimelinesWholeFramesAndKeptEdits() {
        core::Document doc = book();
        (void)footage(doc);
        doc.edit_page(0).extra["anim"] = Json{{"fps", 12}, {"frames", 100000000}, {"loop", true}, {"tracks", Json::array()}};
        QElapsedTimer clock;
        clock.start();
        doc = run(doc, op("add_video_layer", {{"id", "b"}}));
        QCOMPARE(core::video_row(doc.page(0), Json("b"))->at("frames").size(), std::size_t(3000));
        QVERIFY(refusal(doc, op("video_insert_blank", {{"folder", "b"}, {"at", 3000}})).find("longer than 3000") != std::string::npos);
        QVERIFY(clock.elapsed() < 20000);
        core::Document v = video_book();
        for (const Json& at : {Json("2"), Json(2.5), Json(true), Json(-1), Json(5)}) {
            QVERIFY2(refusal(v, op("video_insert_blank", {{"folder", "v"}, {"at", at}})).find("at is a frame") != std::string::npos, at.dump().c_str());
        }
        // (frames 0 1 2 3 at 10 fps: a blank inserted at 2, frame 2 deleted — 0 _ 1 3 —, then at 5 fps: each run laid again)
        v = run(v, op("video_insert_blank", {{"folder", "v"}, {"at", 2}}));
        v = run(v, op("video_delete", {{"folder", "v"}, {"at", 4}}));
        QCOMPARE(frames_of(v), Json::parse("[[0,null],[null,null],[1,null],[3,null]]"));
        v = run(v, op("set_video_layer", {{"folder", "v"}, {"fps", 5}}));
        QCOMPARE(frames_of(v), Json::parse("[[0,null],[0,null],[null,null],[1,null],[1,null],[3,null],[3,null]]"));
    }

    void writtenAndReadBack() {
        core::Document doc = video_book();
        doc = run(doc, op("video_alter", {{"folder", "v"}, {"at", 2}}));
        const fs::path dir = fspath(QStringLiteral("written.genko"));
        genko::test::write_project(doc, dir);
        const std::string bytes = storage::read_file(dir / "project.json");
        QVERIFY(bytes.find("\"video@1\"") != std::string::npos);
        for (const std::string& ref : core::footage_refs(doc)) QVERIFY(storage::AssetStore(dir).has(ref, ".png"));
        const auto back = storage::load_document(dir);
        QVERIFY2(back.document.read_only_reason.empty(), back.document.read_only_reason.c_str());
        QCOMPARE(*core::anim::spec(back.document.page(0)), *core::anim::spec(doc.page(0)));
        QVERIFY(near(middle(back.document, 3), kColours[2]));
        genko::test::write_project(back.document, dir);
        QVERIFY(storage::read_file(dir / "project.json") == bytes);
        // rasterized away: the book names video@1 no more
        core::Document plain = run(back.document, op("rasterize_video", {{"folder", "v"}, {"at", 1}}));
        genko::test::write_project(plain, fspath(QStringLiteral("plain.genko")));
        QVERIFY(storage::read_file(fspath(QStringLiteral("plain.genko")) / "project.json").find("video@1") == std::string::npos);
    }

    // A book without video rows — an animation of cels — as before: frame_page is at_frame, drawn and named the same.
    void celsAsBefore() {
        core::Document doc = book();
        doc = core::CommandBus(render::ops_registry())
                  .apply(doc, Json::parse(R"([{"op": "set_animation", "page": 1, "fps": 8, "frames": 3},
                                             {"op": "add_anim_folder", "page": 1, "id": "f"},
                                             {"op": "add_cel", "page": 1, "folder": "f", "id": "c1", "kind": "paint"},
                                             {"op": "add_cel", "page": 1, "folder": "f", "id": "c2", "at": 2}])"),
                         core::Actor("human:test"))
                  .doc;
        for (std::int64_t frame = 1; frame <= 3; ++frame) {
            const core::Page a = render::footage::frame_page(doc.page(0), frame, &doc), b = core::anim::at_frame(doc.page(0), frame);
            QCOMPARE(a.layers.size(), b.layers.size());
            for (std::size_t i = 0; i < a.layers.size(); ++i) QCOMPARE(a.layers[i].visible, b.layers[i].visible);
        }
        QVERIFY(!core::page_uses_video(doc.page(0)));
        genko::test::write_project(doc, fspath(QStringLiteral("cels.genko")));
        const std::string bytes = storage::read_file(fspath(QStringLiteral("cels.genko")) / "project.json");
        QVERIFY(bytes.find("video@1") == std::string::npos && bytes.find("\"video\"") == std::string::npos);
        // a hand-written "video" of another shape: an anim's before it, kept, named by nothing
        core::Document hand = doc;
        (*hand.edit_page(0).extra.find("anim"))["tracks"][0]["video"] = Json{{"frames", 3}};
        QVERIFY(!core::page_uses_video(hand.page(0)));
        QCOMPARE(render::footage::frame_page(hand.page(0), 2, &hand).layers.size(), hand.page(0).layers.size());
    }

    void writtenOut() {
        core::Document doc = video_book();
        // ビデオをレンダリング: a GIF of what the footage shows
        const fs::path gif = fspath(QStringLiteral("out/video.gif"));
        fs::create_directories(gif.parent_path());
        (void)render::anim::export_animation(doc.page(0), gif, &doc, 40, std::string("frames"));
        for (int frame = 1; frame <= 4; ++frame) {
            char name[32];
            std::snprintf(name, sizeof(name), "frame_%04d.png", frame);
            const render::Image image = render::read_png(storage::read_file(gif / name)).convert("RGB");
            QVERIFY2(near(image.getpixel(image.width() / 2, image.height() / 2), kColours[frame - 1]), name);
        }
        // a .pcraft: the first frame's footage as a layer, the timeline
        std::vector<std::string> notes;
        doc = run(doc, op("set_animation", {{"work", Json::array({2, 3})}}));
        const std::string bytes = formats::pcraft_bytes(doc, 0, {}, &notes);
        QVERIFY(std::any_of(notes.begin(), notes.end(), [](const std::string& n) { return n.find("1 フレーム目") != std::string::npos; }));
        formats::Opened opened = formats::open_file(bytes, "video.pcraft");
        const Json* anim = core::anim::spec(opened.doc.page(0));
        QVERIFY(anim != nullptr);
        QCOMPARE(anim->at("frames"), Json(4));
        QCOMPARE(anim->at("work"), Json::array({2, 3}));
        QCOMPARE(core::anim::fps_of(opened.doc.page(0)), 10.0);
        const QString file = path(QStringLiteral("video.pcraft"));
        genko::test::write_bytes(file, bytes);
        const QString tool = qEnvironmentVariable("GENKO_PCRAFT_CHECK");
        if (tool.isEmpty()) return;
        QProcess process;
        process.start(tool, QStringList{QStringLiteral("check"), file});
        QVERIFY(process.waitForFinished(600000));
        QVERIFY2(process.exitCode() == 0, process.readAllStandardOutput().constData());
        QProcess dump;
        dump.start(tool, QStringList{QStringLiteral("dump"), file});
        QVERIFY(dump.waitForFinished(600000));
        const QByteArray said = dump.readAllStandardOutput();
        QVERIFY2(said.contains("timeline") || said.contains("四色") || said.size() > 0, said.left(2000).constData());
    }
};

QTEST_GUILESS_MAIN(TestContractVideo)
#include "test_contract_video.moc"
