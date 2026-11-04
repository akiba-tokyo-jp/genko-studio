// PC5 J-e2 (video@1, render/footage): a video layer's footage read and laid on its page —
//  - a numbered sequence's files in their names' order (numbers as numbers), a folder's pictures, a file alone;
//  - a sequence read (8 bits; 16 bits kept as 16), a picture shown twice held once, every n-th and a range of it;
//  - every frame of a GIF, an APNG and an animated WebP (this build's own writers' files), and a GIF made by hand: its
//    frames drawn over what the one before left (disposal 2 clears, transparency keeps), a longer frame shown again;
//  - refused: a video file (decision 4), footage past the book's room before anything is decoded or kept, a picture past
//    the limits, a sequence of pictures of other sizes, more than 3000 frames, a range outside;
//  - broken and huge GIF/APNG/WebP never read past their bounds: cut short at every length and with bytes changed, each
//    refused with a core::Error or read; a canvas past the limits refused before its pixels;
//  - a footage frame laid on an 8-bit page (a PNG), a 16-bit page (its precise raster) and a CMYK page (its inks), the page
//    at a frame showing it, remembered, forgotten with the drawing's caches.

#include <QtTest>

#include <QTemporaryDir>

#include <cmath>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/anim.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/video.hpp"
#include "render/codecs/codecs.hpp"
#include "render/footage.hpp"
#include "render/movie.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace fs = std::filesystem;
namespace ft = genko::render::footage;
namespace codecs = genko::render::codecs;
using genko::core::Json;

namespace {

void put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    genko::storage::write_atomic(path, bytes);
}

// A picture of one colour (RGBA, 8 bits).
codecs::CodecImage plain(std::uint32_t w, std::uint32_t h, int r, int g, int b, int a = 255) {
    std::string data;
    for (std::uint32_t i = 0; i < w * h; ++i) {
        data.push_back(static_cast<char>(r));
        data.push_back(static_cast<char>(g));
        data.push_back(static_cast<char>(b));
        data.push_back(static_cast<char>(a));
    }
    return codecs::CodecImage::from_bytes(w, h, codecs::Layout::Rgba, codecs::Sample::U8, data);
}

std::string png(const codecs::CodecImage& image) { return codecs::encode(image, codecs::Format::Png); }

genko::render::Image picture(int w, int h, int r, int g, int b) {
    return genko::render::Image::create("RGBA", genko::render::Size{w, h}, genko::render::Ink{r, g, b, 255}).convert("RGB");
}

// The footage's frames kept here (as a book's assets would be).
struct Held {
    std::map<std::string, std::string> files;
    int stored = 0;
    ft::Store store() {
        return [this](const std::string& bytes) {
            ++stored;
            const std::string ref = genko::storage::AssetStore::ref(bytes);
            files[ref] = bytes;
            return ref;
        };
    }
    codecs::CodecImage frame(const Json& footage, std::size_t i) const { return codecs::decode(files.at(footage["frames"][i].get<std::string>())); }
};

std::string code_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const genko::core::Error& e) {
        return e.code();
    } catch (const std::exception& e) {
        return std::string("other: ") + e.what();
    }
    return {};
}

void le16(std::string& out, unsigned v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>(v >> 8));
}

// A GIF89a of a 4-colour global table (clear, red, green, blue) and these frames: {x, y, w, h, indices, disposal,
// transparent index or -1, delay in 1/100 s}.
struct GifFrame {
    unsigned x, y, w, h;
    std::vector<std::uint8_t> indices;
    int disposal = 0;
    int transparent = -1;
    int delay = 10;
};
std::string gif(unsigned w, unsigned h, const std::vector<GifFrame>& frames) {
    std::string out("GIF89a", 6);
    le16(out, w);
    le16(out, h);
    out.push_back(static_cast<char>(0x81));  // (a global table of 4)
    out.push_back('\0');
    out.push_back('\0');
    for (const int c : {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255}) out.push_back(static_cast<char>(c));
    for (const GifFrame& f : frames) {
        out += std::string("\x21\xF9\x04", 3);
        out.push_back(static_cast<char>((f.disposal << 2) | (f.transparent >= 0 ? 1 : 0)));
        le16(out, static_cast<unsigned>(f.delay));
        out.push_back(static_cast<char>(f.transparent >= 0 ? f.transparent : 0));
        out.push_back('\0');
        out.push_back('\x2C');
        le16(out, f.x);
        le16(out, f.y);
        le16(out, f.w);
        le16(out, f.h);
        out.push_back('\0');
        out.push_back('\x02');
        out += genko::render::movie::gif_lzw(f.indices, 2);
    }
    out.push_back('\x3B');
    return out;
}

}  // namespace

class TestFootage : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    fs::path at(const std::string& rel) const { return fs::path(tmp_.path().toStdString()) / rel; }

    // Three frames (red, green, blue, 16 × 8) written by this build's moving pictures' writer.
    fs::path moving(const std::string& format) {
        const fs::path out = at("moving/frames." + format);
        fs::create_directories(out.parent_path());
        (void)genko::render::movie::write_movie({picture(16, 8, 255, 0, 0), picture(16, 8, 0, 255, 0), picture(16, 8, 0, 0, 255)}, out, 8, format);
        return out;
    }

private slots:
    void initTestCase() { QVERIFY(tmp_.isValid()); }

    void sequenceNamesInOrder() {
        for (const char* name : {"a1.png", "a2.png", "a10.png", "a3.jpg", "b1.png", "a.png", "notes.txt"}) put(at(std::string("names/") + name), "x");
        const auto names = [](const std::vector<fs::path>& files) {
            std::vector<std::string> out;
            for (const auto& f : files) out.push_back(f.filename().string());
            return out;
        };
        QCOMPARE(names(ft::sequence_files(at("names/a1.png"), true)), (std::vector<std::string>{"a1.png", "a2.png", "a10.png"}));
        QCOMPARE(names(ft::sequence_files(at("names/a2.png"), false)), (std::vector<std::string>{"a2.png"}));
        QCOMPARE(names(ft::sequence_files(at("names/a.png"), true)), (std::vector<std::string>{"a.png"}));
        QCOMPARE(names(ft::sequence_files(at("names"), true)), (std::vector<std::string>{"a.png", "a1.png", "a2.png", "a3.jpg", "a10.png", "b1.png"}));
    }

    void sequenceRead() {
        put(at("seq/f_001.png"), png(plain(4, 3, 255, 0, 0)));
        put(at("seq/f_002.png"), png(plain(4, 3, 0, 0, 255, 128)));
        put(at("seq/f_003.png"), png(plain(4, 3, 255, 0, 0)));
        Held held;
        const ft::Footage read = ft::read_footage(at("seq/f_001.png"), {}, held.store());
        const Json& f = read.json;
        QCOMPARE(f["kind"], Json("sequence"));
        QCOMPARE(f["frames"].size(), std::size_t(3));
        QCOMPARE(f["frames"][0], f["frames"][2]);  // (the same picture held once)
        QCOMPARE(held.files.size(), std::size_t(2));
        QCOMPARE(f["size_px"], Json::array({4, 3}));
        QCOMPARE(f["depth"], Json(8));
        QCOMPARE(f["fps"], Json(30.0));
        QCOMPARE(f["alpha"], Json("straight"));
        QCOMPARE(f["source"]["path"].get<std::string>(), fs::absolute(at("seq/f_001.png")).string());
        QCOMPARE(f["source"]["sha256"].get<std::string>().size(), std::size_t(64));
        genko::core::check_video(Json{{"footage", f}, {"frames", Json::array()}, {"show_altered", true}, {"placement_mm", Json::array({0, 0, 10, 10})}, {"fit", "contain"}});
        const codecs::CodecImage second = held.frame(f, 1);
        QCOMPARE(second.sample(), codecs::Sample::U8);
        QCOMPARE(std::lround(second.at(1, 1, 2) * 255), 255L);
        QCOMPARE(std::lround(second.at(1, 1, 3) * 255), 128L);
        // every n-th, a range, a folder
        ft::ReadOptions some;
        some.every = 2;
        QCOMPARE(ft::read_footage(at("seq/f_001.png"), some, held.store()).json["frames"].size(), std::size_t(2));
        some.every = 1;
        some.range = std::array<std::int64_t, 2>{2, 3};
        const Json part = ft::read_footage(at("seq"), some, held.store()).json;
        QCOMPARE(part["frames"].size(), std::size_t(2));
        QCOMPARE(part["kind"], Json("sequence"));
        QCOMPARE(part["name"], Json("seq"));
        some.range = std::array<std::int64_t, 2>{0, 3};
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("seq"), some, held.store()); }), std::string("value"));
        some.range.reset();
        some.fps = 12.5;
        QCOMPARE(ft::read_footage(at("seq/f_002.png"), some, held.store()).json["fps"], Json(12.5));
        // one picture
        ft::ReadOptions alone;
        alone.sequence = false;
        const Json one = ft::read_footage(at("seq/f_002.png"), alone, held.store()).json;
        QCOMPARE(one["kind"], Json("image"));
        QCOMPARE(one["frames"].size(), std::size_t(1));
    }

    void deepSequenceKeepsItsBits() {
        std::string data;
        for (int i = 0; i < 6; ++i) {
            for (const std::uint16_t v : {std::uint16_t(1000), std::uint16_t(40000), std::uint16_t(65535)}) data.append(reinterpret_cast<const char*>(&v), 2);
        }
        put(at("deep/d1.png"), png(codecs::CodecImage::from_bytes(3, 2, codecs::Layout::Rgb, codecs::Sample::U16, data)));
        put(at("deep/d2.png"), png(codecs::CodecImage::from_bytes(3, 2, codecs::Layout::Rgb, codecs::Sample::U16, data)));
        Held held;
        const Json f = ft::read_footage(at("deep/d1.png"), {}, held.store()).json;
        QCOMPARE(f["depth"], Json(16));
        const codecs::CodecImage frame = held.frame(f, 0);
        QCOMPARE(frame.sample(), codecs::Sample::U16);
        QCOMPARE(frame.layout(), codecs::Layout::Rgba);
        QCOMPARE(std::lround(frame.at(2, 1, 0) * 65535), 1000L);
        QCOMPARE(std::lround(frame.at(2, 1, 1) * 65535), 40000L);
        QCOMPARE(std::lround(frame.at(2, 1, 3) * 65535), 65535L);
    }

    void everyFrameOfMovingPictures() {
        for (const std::string format : {"gif", "png", "webp"}) {
            Held held;
            const Json f = ft::read_footage(moving(format), {}, held.store()).json;
            QCOMPARE(f["kind"], Json("animation"));
            QCOMPARE(f["size_px"], Json::array({16, 8}));
            QCOMPARE(f["depth"], Json(8));
            QVERIFY2(f["frames"].size() == 3, (format + " " + f.dump()).c_str());
            QVERIFY2(std::fabs(f["fps"].get<double>() - 8) < 0.5, (format + " " + f.dump()).c_str());
            const int expect[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
            for (std::size_t i = 0; i < 3; ++i) {
                const codecs::CodecImage frame = held.frame(f, i);
                for (int c = 0; c < 3; ++c) {
                    const long got = std::lround(frame.at(8, 4, static_cast<unsigned>(c)) * 255);
                    QVERIFY2(std::labs(got - expect[i][c]) <= (format == "webp" ? 24 : 0), (format + " frame " + std::to_string(i)).c_str());
                }
            }
        }
    }

    // A GIF made by hand: a red canvas cleared after it (disposal 2), then a green half over clear (its transparent entry
    // keeping what is under it), the second shown twice as long (laid twice), a third frame shown under 2/100 s as 1/10 s.
    void gifFramesDrawnOverEachOther() {
        const std::string bytes = gif(4, 2, {GifFrame{0, 0, 4, 2, {1, 1, 1, 1, 1, 1, 1, 1}, 2, -1, 10},
                                             GifFrame{2, 0, 2, 2, {2, 0, 2, 2}, 1, 0, 20},
                                             GifFrame{0, 0, 1, 1, {3}, 0, -1, 0}});
        put(at("gif/hand.gif"), bytes);
        Held held;
        const Json f = ft::read_footage(at("gif/hand.gif"), {}, held.store()).json;
        QCOMPARE(f["fps"], Json(10.0));
        QCOMPARE(f["frames"].size(), std::size_t(4));
        QCOMPARE(f["frames"][1], f["frames"][2]);
        const codecs::CodecImage first = held.frame(f, 0), second = held.frame(f, 1), third = held.frame(f, 3);
        QCOMPARE(std::lround(first.at(0, 0, 0) * 255), 255L);
        QCOMPARE(std::lround(first.at(0, 0, 3) * 255), 255L);
        QCOMPARE(std::lround(second.at(0, 0, 3) * 255), 0L);   // (cleared by the first's disposal)
        QCOMPARE(std::lround(second.at(2, 0, 1) * 255), 255L);  // (green)
        QCOMPARE(std::lround(second.at(3, 0, 3) * 255), 0L);   // (its transparent entry: what was under it, clear)
        QCOMPARE(std::lround(third.at(0, 0, 2) * 255), 255L);  // (blue over the second frame left as it was)
        QCOMPARE(std::lround(third.at(2, 1, 1) * 255), 255L);
    }

    void refused() {
        Held held;
        put(at("refused/clip.mp4"), "....ftypisom");
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("refused/clip.mp4"), {}, held.store()); }), std::string("video_file"));
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("refused/none.png"), {}, held.store()); }), std::string("io"));
        put(at("refused/text.png"), "not a picture");
        QVERIFY(!code_of([&] { (void)ft::read_footage(at("refused/text.png"), {}, held.store()); }).empty());
        // past the book's room: before anything is decoded or kept
        ft::ReadOptions tight;
        tight.most_bytes = 16 * 8 * 4 * 3 - 1;
        QCOMPARE(code_of([&] { (void)ft::read_footage(moving("gif"), tight, held.store()); }), std::string("footage_too_large"));
        put(at("refused/r1.png"), png(plain(8, 8, 1, 2, 3)));
        put(at("refused/r2.png"), png(plain(8, 9, 1, 2, 3)));
        tight.most_bytes = 8 * 8 * 4 * 2 - 1;
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("refused/r1.png"), tight, held.store()); }), std::string("footage_too_large"));
        QCOMPARE(held.stored, 0);
        // pictures of other sizes
        const std::string other = code_of([&] { (void)ft::read_footage(at("refused/r1.png"), {}, held.store()); });
        QCOMPARE(other, std::string("value"));
        // past a picture's limits
        ft::ReadOptions small;
        small.limits.max_pixels = 10;
        QCOMPARE(code_of([&] { (void)ft::read_footage(moving("png"), small, held.store()); }), std::string("image_too_large"));
        // more than 3000 frames
        std::vector<GifFrame> many(3001, GifFrame{0, 0, 1, 1, {1}, 0, -1, 10});
        put(at("refused/many.gif"), gif(1, 1, many));
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("refused/many.gif"), {}, held.store()); }), std::string("value"));
        ft::ReadOptions half;
        half.every = 2;
        QCOMPARE(ft::read_footage(at("refused/many.gif"), half, held.store()).json["frames"].size(), std::size_t(1501));
        ft::ReadOptions speed;
        speed.fps = 200;
        QCOMPARE(code_of([&] { (void)ft::read_footage(moving("gif"), speed, held.store()); }), std::string("value"));
    }

    void brokenNeverReadPastTheirBounds() {
        const std::string hand = gif(4, 2, {GifFrame{0, 0, 4, 2, {1, 1, 1, 1, 2, 2, 2, 2}, 0, -1, 10}, GifFrame{1, 1, 2, 1, {3, 3}, 0, -1, 10}});
        std::vector<std::string> files{hand};
        for (const std::string format : {"gif", "png", "webp"}) files.push_back(genko::storage::read_file(moving(format)));
        std::mt19937 random(20261103);
        int read = 0, refused = 0;
        for (std::size_t k = 0; k < files.size(); ++k) {
            const std::string& whole = files[k];
            std::vector<std::string> broken;
            for (std::size_t n = 0; n < whole.size(); n += std::max<std::size_t>(1, whole.size() / 97)) broken.push_back(whole.substr(0, n));
            for (int i = 0; i < 300; ++i) {
                std::string changed = whole;
                for (int j = 0; j < 1 + i % 4; ++j) changed[random() % changed.size()] = static_cast<char>(random() & 0xFF);
                broken.push_back(changed);
            }
            for (std::size_t i = 0; i < broken.size(); ++i) {
                const fs::path file = at("broken/" + std::to_string(k) + "_" + std::to_string(i) + (k == 0 || k == 1 ? ".gif" : k == 2 ? ".png" : ".webp"));
                put(file, broken[i]);
                Held held;
                const std::string code = code_of([&] { (void)ft::read_footage(file, {}, held.store()); });
                QVERIFY2(code.rfind("other", 0) != 0, (file.string() + ": " + code).c_str());
                (code.empty() ? read : refused) += 1;
                fs::remove(file);
            }
        }
        QVERIFY(refused > 0 && read > 0);
        // a canvas past the limits: refused before its pixels
        std::string huge = hand;
        huge[6] = huge[7] = huge[8] = huge[9] = static_cast<char>(0xFF);
        put(at("broken/huge.gif"), huge);
        Held held;
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("broken/huge.gif"), {}, held.store()); }), std::string("image_too_large"));
        std::string apng = genko::storage::read_file(moving("png"));
        for (int i = 0; i < 4; ++i) apng[16 + i] = apng[20 + i] = static_cast<char>(i == 1 ? 0x7F : 0);  // (IHDR's sides: 8 million)
        put(at("broken/huge.png"), apng);
        QCOMPARE(code_of([&] { (void)ft::read_footage(at("broken/huge.png"), {}, held.store()); }), std::string("image_too_large"));
        QCOMPARE(held.stored, 0);
    }

    // A footage frame laid on an 8-bit, a 16-bit and a CMYK page; the page at its frame showing it; remembered.
    void laidOnThePages() {
        genko::core::Document doc = genko::core::new_episode("ビデオ", genko::core::Num(1), 3,
                                                             genko::core::PageSpec::custom(96, 72, 20, 20, 1, 2, 2, 2, 2, 200, "color"));
        for (std::size_t i = 0; i < 3; ++i) doc.edit_page(i).frames.clear();
        put(at("laid/c1.png"), png(plain(40, 30, 255, 0, 0)));
        put(at("laid/c2.png"), png(plain(40, 30, 0, 0, 255)));
        const Json footage = ft::read_footage(at("laid/c1.png"), {}, ft::memory_store(doc)).json;
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const genko::core::Actor actor("human:test");
        Json ops = Json::array({Json{{"op", "set_color_mode"}, {"page", 2}, {"mode", "rgb"}, {"depth", 16}},
                                Json{{"op", "set_color_mode"}, {"page", 3}, {"mode", "cmyk"}, {"profile", "coated-cmyk"}}});
        for (int p = 1; p <= 3; ++p) ops.push_back(Json{{"op", "add_video_layer"}, {"page", p}, {"footage", footage}, {"id", "v" + std::to_string(p)}});
        doc = bus.apply(doc, ops, actor, false).doc;
        genko::render::footage::clear_cache();
        for (std::size_t i = 0; i < 3; ++i) {
            const genko::core::Page& page = doc.page(i);
            const Json* video = genko::core::video_row(page, Json("v" + std::to_string(i + 1)));
            QVERIFY(video != nullptr);
            const genko::core::Layer layer = ft::footage_layer(doc, page, *video, 1);
            QVERIFY(i == 0 ? bool(layer.raster_png) : i == 1 ? bool(layer.color_raster) : bool(layer.ink_raster));
            const genko::core::Page shown = ft::frame_page(page, 2, &doc);
            QCOMPARE(shown.layers.size(), page.layers.size() + 1);
            genko::render::RenderOptions options;
            options.mode = "proof";
            options.at_frame = true;
            const genko::render::Image drawn = genko::render::render_page(shown, 50, options, &doc).image.convert("RGB");
            const auto px = drawn.getpixel(drawn.width() / 2, drawn.height() / 2);
            // (blue: on the CMYK page as its inks show it, out of the inks' gamut)
            const bool blue = i < 2 ? px[2] > 200 && px[0] < 60 : px[2] > px[0] + 50 && px[2] > px[1] + 30;
            QVERIFY2(blue, (std::to_string(i) + ": " + std::to_string(px[0]) + " " + std::to_string(px[1]) + " " + std::to_string(px[2])).c_str());
        }
        const auto made = ft::layers_made();
        (void)ft::frame_page(doc.page(0), 1, &doc);
        (void)ft::frame_page(doc.page(0), 2, &doc);
        QCOMPARE(ft::layers_made(), made + 1);  // (frame 2 remembered; frame 1 made once)
        QVERIFY(ft::cache_bytes() > 0);
        genko::render::clear_render_caches();
        QCOMPARE(ft::cache_bytes(), std::int64_t(0));
    }
};

QTEST_GUILESS_MAIN(TestFootage)
#include "test_footage.moc"
