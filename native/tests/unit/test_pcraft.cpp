// The .pcraft's parts (PC4 F-f, SPEC.md FILE-01) against PhotoCraft v0.2.0's own tests of crates/format (src/zip.rs: the
// zip's round trip and CRC; tests/corruption.rs: a file cut at any point, bytes flipped, a tile missing or swapped, a
// manifest not JSON or of a newer version, a hash that is not one, the manifest's and the whole's limits, a folder bundle
// tampered with, a bundle zipped again with deflate) on this build's reader, with the bundles upstream's writer made
// (native/tests/data/pcraft, tools/migration/pcraft_fixtures) — and what upstream's tests do not ask: BLAKE3 against the
// hashes upstream's crate named the tiles by, zstd frames that say or unpack to more than they may (a zstd bomb), files of
// a zip that say they are larger than they may be (a zip bomb), a canvas past the picture limits and tiles past the
// budget, all refused before they are made.

#include <QtTest>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <zstd.h>

#include "core/error.hpp"
#include "core/json.hpp"
#include "render/blake3.hpp"
#include "render/pcraft_manifest.hpp"
#include "render/pcraft_production.hpp"
#include "render/pcraft_read.hpp"
#include "render/zip.hpp"
#include "render/zstd.hpp"
#include "testsupport.hpp"

namespace pc = genko::render::pcraft;
namespace zip = genko::render::zip;
namespace zstd = genko::render::zstd;
namespace b3 = genko::render::blake3;
using genko::core::Json;
namespace gt = genko::test;

namespace {

std::string fixture(const char* name) { return gt::read_bytes(gt::test_data(QStringLiteral("pcraft/") + QString::fromUtf8(name))); }

// The archive's files (name → bytes) as written, in order.
std::vector<std::pair<std::string, std::string>> files_of(const std::string& bytes) {
    const zip::NamedReader r(bytes);
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& name : r.names()) out.emplace_back(name, r.read(name, ~0ull));
    return out;
}

// A new archive of the files, each changed (or left out: none) by `f`.
std::string rebuilt(const std::string& bytes, const std::function<std::optional<std::string>(const std::string&, std::string)>& f) {
    std::string out;
    zip::StoreWriter w([&out](std::string_view b) { out.append(b); });
    for (auto& [name, data] : files_of(bytes)) {
        if (auto changed = f(name, std::move(data))) w.add(name, *changed);
    }
    w.finish();
    return out;
}

std::string code_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const genko::core::Error& error) {
        return error.code();
    } catch (const std::exception&) {
        return "other";
    }
    return "";
}

std::string message_of(const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& error) {
        return error.what();
    }
    return "";
}

// A zstd frame of `n` zero bytes; without its size in the header when `streamed` (as a writer that does not know it).
std::string zeros_frame(std::size_t n, bool streamed) {
    if (!streamed) return zstd::compress(std::string(n, '\0'), 1);
    ZSTD_CCtx* ctx = ZSTD_createCCtx();
    std::string out(ZSTD_compressBound(n) + 1024, '\0');
    const std::string zeros(1 << 20, '\0');
    ZSTD_outBuffer o{out.data(), out.size(), 0};
    std::size_t left = n;
    while (left > 0) {
        const std::size_t take = std::min(left, zeros.size());
        ZSTD_inBuffer in{zeros.data(), take, 0};
        while (in.pos < in.size) ZSTD_compressStream2(ctx, &o, &in, ZSTD_e_continue);
        left -= take;
    }
    ZSTD_inBuffer none{nullptr, 0, 0};
    while (ZSTD_compressStream2(ctx, &o, &none, ZSTD_e_end) != 0) {
    }
    ZSTD_freeCCtx(ctx);
    out.resize(o.pos);
    return out;
}

// A bundle of a manifest's text (and these objects) only.
std::string bundle_text(const std::string& manifest) {
    std::string out;
    zip::StoreWriter w([&out](std::string_view b) { out.append(b); });
    w.add("manifest.json", manifest);
    w.finish();
    return out;
}

// A bundle of a manifest (and these objects) only.
std::string bundle(const Json& manifest, const std::vector<std::pair<std::string, std::string>>& objects = {}) {
    std::string out;
    zip::StoreWriter w([&out](std::string_view b) { out.append(b); });
    w.add("manifest.json", manifest.dump());
    for (const auto& [name, data] : objects) w.add(name, data);
    w.finish();
    return out;
}

Json small_doc(int w, int h) {
    return Json{{"format_version", 1},
                {"generator", "test"},
                {"document",
                 Json{{"id", 1},
                      {"name", "t"},
                      {"size", Json{{"width", w}, {"height", h}}},
                      {"resolution_dpi", 72.0},
                      {"mode", "Rgb"},
                      {"depth", "U8"},
                      {"icc_profile", nullptr},
                      {"layers", Json::array()},
                      {"channels", Json::array()},
                      {"guides", Json{{"horizontal", Json::array()}, {"vertical", Json::array()}}},
                      {"selection", nullptr},
                      {"metadata", Json{{"xmp", nullptr}, {"exif", nullptr}, {"psd_resources", Json::array()}, {"psd_global_blocks", Json::array()}}},
                      {"global_light", Json{{"angle", 120.0}, {"altitude", 30.0}}}}}};
}

Json raster_layer(const Json& tiles) {
    return Json{{"id", 2},
                {"name", "L"},
                {"visible", true},
                {"locks", Json{{"transparency", false}, {"pixels", false}, {"position", false}, {"artboard", false}, {"all", false}}},
                {"blend", "Normal"},
                {"opacity", 1.0},
                {"fill_opacity", 1.0},
                {"clipped", false},
                {"mask", nullptr},
                {"effects", Json{{"enabled", true}, {"items", Json::array()}, {"psd_raw", nullptr}}},
                {"label", "None"},
                {"content", Json{{"kind", "raster"},
                                 {"surface", Json{{"format", Json{{"mode", "Rgb"}, {"sample", "U8"}, {"alpha", true}}}, {"default", "00000000"}, {"tiles", tiles}}}}},
                {"psd_blocks", Json::array()},
                {"psd_id", nullptr},
                {"fill_cache", nullptr}};
}

}  // namespace

class TestPcraft : public QObject {
    Q_OBJECT
private slots:
    // BLAKE3: the empty input's published hash, and the names upstream's crate gave its tiles (their content's hash).
    void blake3Hashes() {
        QCOMPARE(b3::hex(""), std::string("af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));
        QCOMPARE(b3::version(), std::string("1.8.7"));
        QVERIFY(b3::is_hex_hash(b3::hex("abc")));
        QVERIFY(!b3::is_hex_hash("AF1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));
        QVERIFY(!b3::is_hex_hash("af1349"));
        QVERIFY(!b3::is_hex_hash("../1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"));
        int tiles = 0;
        for (const auto& [name, data] : files_of(fixture("rich_rgb8.pcraft"))) {
            if (!name.starts_with("tiles/")) continue;
            const std::string hash = name.substr(6, 64);
            const std::size_t said = ZSTD_getFrameContentSize(data.data(), data.size());
            const std::string raw = said == ZSTD_CONTENTSIZE_UNKNOWN ? zstd::decompress_bounded(data, 1 << 24) : zstd::decompress_exact(data, said);
            QCOMPARE(b3::hex(raw), hash);
            ++tiles;
        }
        QVERIFY(tiles > 4);
    }

    void zstdRoundTripAndBounds() {
        std::string bytes;
        for (int i = 0; i < 100000; ++i) bytes.push_back(static_cast<char>((i * 7) % 251));
        const std::string frame = zstd::compress(bytes, 1);
        QCOMPARE(zstd::decompress_exact(frame, bytes.size()), bytes);
        QCOMPARE(zstd::decompress_bounded(frame, bytes.size()), bytes);
        QCOMPARE(code_of([&] { zstd::decompress_exact(frame, bytes.size() - 1); }), std::string("format"));
        QCOMPARE(code_of([&] { zstd::decompress_exact(frame, bytes.size() + 1); }), std::string("format"));
        QCOMPARE(code_of([&] { zstd::decompress_exact(frame.substr(0, frame.size() / 2), bytes.size()); }), std::string("format"));
        QCOMPARE(code_of([&] { zstd::decompress_exact(frame + "x", bytes.size()); }), std::string("format"));
        QCOMPARE(code_of([&] { zstd::decompress_exact("not zstd at all", 10); }), std::string("format"));
        QCOMPARE(code_of([&] { zstd::decompress_bounded(frame, bytes.size() - 1); }), std::string("image_too_large"));
        // a bomb: 256 MiB of zeros in a few kilobytes — its header says so (refused before), or does not (refused while
        // it unpacks, never past the limit)
        const std::string said = zeros_frame(256u << 20, false), streamed = zeros_frame(256u << 20, true);
        QVERIFY(said.size() < (64u << 10) && streamed.size() < (64u << 10));
        QCOMPARE(code_of([&] { zstd::decompress_bounded(said, 1 << 20); }), std::string("image_too_large"));
        QCOMPARE(code_of([&] { zstd::decompress_bounded(streamed, 1 << 20); }), std::string("image_too_large"));
        QCOMPARE(code_of([&] { zstd::decompress_exact(streamed, 1 << 20); }), std::string("format"));
        QCOMPARE(zstd::decompress_bounded(zeros_frame(3u << 20, true), 3u << 20).size(), std::size_t(3u << 20));
    }

    void zipRoundTripAndChecks() {
        std::string out;
        zip::StoreWriter w([&out](std::string_view b) { out.append(b); });
        w.add("a.txt", "hello");
        w.add("dir/b.bin", std::string("\0\1\2\3", 4));
        w.add("empty", "");
        w.finish();
        QCOMPARE(w.written(), static_cast<std::uint64_t>(out.size()));
        const zip::NamedReader r(out);
        QCOMPARE(r.names(), (std::vector<std::string>{"a.txt", "dir/b.bin", "empty"}));
        QCOMPARE(r.read("a.txt", 100), std::string("hello"));
        QCOMPARE(r.read("dir/b.bin", 100), std::string("\0\1\2\3", 4));
        QCOMPARE(r.read("empty", 0), std::string());
        QCOMPARE(*r.unpacked_size("a.txt"), std::uint64_t(5));
        QCOMPARE(code_of([&] { r.read("a.txt", 3); }), std::string("image_too_large"));
        QCOMPARE(code_of([&] { r.read("missing", 3); }), std::string("format"));
        // a CRC that does not match: refused
        std::string bad = out;
        bad[bad.find("hello")] = 'j';
        QCOMPARE(code_of([&] { zip::NamedReader(bad).read("a.txt", 100); }), std::string("format"));
        // deflated files (another tool's zip) are read
        const std::string deflated = zip::archive({{"x.txt", std::string(5000, 'x'), true}, {"y", "why", true}});
        QCOMPARE(zip::NamedReader(deflated).read("x.txt", 5000), std::string(5000, 'x'));
        // a file that says it unpacks past what may be read: refused before it is unpacked (a zip bomb)
        std::string bomb = deflated;
        const std::size_t cd = bomb.rfind(std::string("PK\1\2", 4));
        const std::uint32_t huge = 0xF0000000u;
        std::memcpy(bomb.data() + cd + 24, &huge, 4);
        QCOMPARE(code_of([&] { zip::NamedReader(bomb).read("y", 1u << 30); }), std::string("image_too_large"));
        // not a zip, cut, encrypted
        QCOMPARE(code_of([] { zip::NamedReader("not a zip"); }), std::string("format"));
        QCOMPARE(code_of([&] { zip::NamedReader(out.substr(0, out.size() - 3)); }), std::string("format"));
        std::string locked = out;
        const std::size_t first = locked.find(std::string("PK\1\2", 4));
        locked[first + 8] = 1;
        QCOMPARE(code_of([&] { zip::NamedReader x(locked); }), std::string("format"));
        // past what a zip without zip64 holds: refused before the file is written
        std::string many;
        zip::StoreWriter m([&many](std::string_view b) { many.append(b); });
        for (int i = 0; i < 65535; ++i) m.add(std::to_string(i), "");
        QCOMPARE(code_of([&] { m.add("one more", ""); }), std::string("image_too_large"));
    }

    // upstream's corruption tests on a bundle its writer made
    void truncationNeverPassesAndNeverCrashes() {
        const std::string b = fixture("rich_rgb8.pcraft");
        QVERIFY(pc::is_pcraft(b));
        (void)pc::read(b, "Rich");  // (whole: it reads)
        const std::size_t step = std::max<std::size_t>(1, b.size() / 200);
        for (std::size_t cut = 0; cut < b.size(); cut += step) {
            const std::string code = code_of([&] { (void)pc::read(std::string_view(b).substr(0, cut), "x"); });
            QVERIFY2(!code.empty() && code != "other", qPrintable(QStringLiteral("cut at %1: %2").arg(cut).arg(QString::fromStdString(code))));
        }
    }

    void byteFlipsAreFound() {
        const std::string b = fixture("rich_rgb8.pcraft");
        std::uint64_t state = 9;
        int failures = 0;
        for (int i = 0; i < 300; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            std::string c = b;
            c[(state >> 33) % c.size()] ^= 0x20;
            const std::string code = code_of([&] { (void)pc::read(c, "x"); });
            QVERIFY(code != "other");
            if (!code.empty()) ++failures;
        }
        QVERIFY2(failures > 200, qPrintable(QString::number(failures)));
    }

    void missingAndSwappedTiles() {
        const std::string b = fixture("rich_rgb8.pcraft");
        const std::string missing = rebuilt(b, [](const std::string& n, std::string d) -> std::optional<std::string> {
            if (n.starts_with("tiles/")) return std::nullopt;
            return d;
        });
        QCOMPARE(code_of([&] { (void)pc::read(missing, "x"); }), std::string("format"));
        std::vector<std::string> tiles;
        for (const auto& [n, d] : files_of(b)) {
            if (n.starts_with("tiles/")) tiles.push_back(n);
        }
        const std::string other = zip::NamedReader(b).read(tiles[1], ~0ull);
        const std::string swapped = rebuilt(b, [&](const std::string& n, std::string d) -> std::optional<std::string> { return n == tiles[0] ? other : d; });
        const std::string why = message_of([&] { (void)pc::read(swapped, "x"); });
        QVERIFY2(why.find("hash") != std::string::npos || why.find("zstd") != std::string::npos, why.c_str());
    }

    void badManifests() {
        const std::string b = fixture("rich_rgb8.pcraft");
        const auto with_manifest = [&](const std::function<std::string(std::string)>& f) {
            return rebuilt(b, [&](const std::string& n, std::string d) -> std::optional<std::string> { return n == "manifest.json" ? f(std::move(d)) : d; });
        };
        QCOMPARE(code_of([&] { (void)pc::read(with_manifest([](std::string) { return std::string("{not json"); }), "x"); }), std::string("format"));
        QCOMPARE(code_of([&] { (void)pc::read(rebuilt(b, [](const std::string& n, std::string d) -> std::optional<std::string> {
                                    if (n == "manifest.json") return std::nullopt;
                                    return d;
                                }), "x"); }),
                 std::string("format"));
        const std::string newer = with_manifest([](std::string d) {
            Json v = Json::parse(d);
            v["format_version"] = 999;
            return v.dump();
        });
        QCOMPARE(code_of([&] { (void)pc::read(newer, "x"); }), std::string("unsupported_version"));
        QVERIFY(message_of([&] { (void)pc::read(newer, "x"); }).find("format 999") != std::string::npos);
        const std::string zero = with_manifest([](std::string d) {
            Json v = Json::parse(d);
            v["format_version"] = 0;
            return v.dump();
        });
        QCOMPARE(code_of([&] { (void)pc::read(zero, "x"); }), std::string("format"));
        const std::string not_hash = with_manifest([](std::string d) {
            const std::size_t at = d.find("\"hash\": \"");
            d[at + 9] = '/';
            return d;
        });
        QCOMPARE(code_of([&] { (void)pc::read(not_hash, "x"); }), std::string("format"));
    }

    void limits() {
        const std::string b = fixture("rich_rgb8.pcraft");
        pc::ReadLimits small;
        small.max_manifest_bytes = 100;
        QCOMPARE(code_of([&] { (void)pc::read(b, "x", small); }), std::string("image_too_large"));
        pc::ReadLimits total;
        total.max_unpacked_bytes = 1000;
        QCOMPARE(code_of([&] { (void)pc::read(b, "x", total); }), std::string("image_too_large"));
        // a canvas past the picture limits: refused before anything is made
        QCOMPARE(code_of([&] { (void)pc::read(bundle(small_doc(60000, 60000)), "x"); }), std::string("image_too_large"));
        QCOMPARE(code_of([&] { (void)pc::read(bundle(small_doc(0, 10)), "x"); }), std::string("format"));
        // a thousand tiles of one zero frame (256 KiB each unpacked, a few hundred bytes packed): past the budget before
        // they are unpacked
        const std::string tile(256 * 256 * 4, '\0');
        const std::string hash = b3::hex(tile);
        Json tiles = Json::array();
        for (int ty = 0; ty < 32; ++ty) {
            for (int tx = 0; tx < 32; ++tx) tiles.push_back(Json{{"tx", tx}, {"ty", ty}, {"hash", hash}});
        }
        Json m = small_doc(8192, 8192);
        m["document"]["layers"].push_back(raster_layer(tiles));
        const std::string many = bundle(m, {{"tiles/" + hash + ".zst", zstd::compress(tile, 1)}});
        pc::ReadLimits budget;
        budget.max_unpacked_bytes = 64u << 20;
        QCOMPARE(code_of([&] { (void)pc::read(many, "x", budget); }), std::string("image_too_large"));
        // a blob bomb (an ICC profile of 512 MiB of zeros): refused, never unpacked whole
        const std::string blob = zeros_frame(512u << 20, true);
        const std::string fake = b3::hex("not these bytes");
        Json p = small_doc(16, 16);
        p["document"]["icc_profile"] = fake;
        QCOMPARE(code_of([&] { (void)pc::read(bundle(p, {{"blobs/" + fake + ".zst", blob}}), "x"); }), std::string("image_too_large"));
        // a tile of the wrong size for its format
        Json q = small_doc(16, 16);
        q["document"]["layers"].push_back(raster_layer(Json::array({Json{{"tx", 0}, {"ty", 0}, {"hash", b3::hex("tiny")}}})));
        QCOMPARE(code_of([&] { (void)pc::read(bundle(q, {{"tiles/" + b3::hex("tiny") + ".zst", zstd::compress("tiny", 1)}}), "x"); }), std::string("format"));
        // the same tile twice
        Json twice = small_doc(16, 16);
        twice["document"]["layers"].push_back(raster_layer(Json::array({Json{{"tx", 0}, {"ty", 0}, {"hash", hash}}, Json{{"tx", 0}, {"ty", 0}, {"hash", hash}}})));
        QCOMPARE(code_of([&] { (void)pc::read(bundle(twice, {{"tiles/" + hash + ".zst", zstd::compress(tile, 1)}}), "x"); }), std::string("format"));
    }

    void foldersAndDeflate() {
        const std::string b = fixture("rich_rgb8.pcraft");
        const pc::Read whole = pc::read(b, "Rich");
        QTemporaryDir dir;
        const std::filesystem::path root(dir.path().toStdString());
        for (const auto& [name, data] : files_of(b)) {
            std::filesystem::create_directories((root / name).parent_path());
            gt::write_bytes(QString::fromStdString((root / name).string()), data);
        }
        const pc::Read folder = pc::read_folder(root, "Rich");
        QCOMPARE(folder.doc.page(0).layers.size(), whole.doc.page(0).layers.size());
        QCOMPARE(folder.notes, whole.notes);
        // a tile tampered with in the folder
        for (const auto& entry : std::filesystem::directory_iterator(root / "tiles")) {
            gt::write_bytes(QString::fromStdString(entry.path().string()), "garbage");
            break;
        }
        QVERIFY(!code_of([&] { (void)pc::read_folder(root, "x"); }).empty());
        // zipped again with deflate: still read, the same
        std::vector<zip::Entry> entries;
        for (auto& [name, data] : files_of(b)) entries.push_back(zip::Entry{name, std::move(data), true});
        const pc::Read again = pc::read(zip::archive(entries), "Rich");
        QCOMPARE(again.doc.page(0).layers.size(), whole.doc.page(0).layers.size());
        QCOMPARE(again.notes, whole.notes);
    }

    void randomBytesNeverCrash() {
        std::uint64_t state = 77;
        for (int i = 0; i < 300; ++i) {
            std::string bytes;
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t n = (state >> 33) % 4096;
            for (std::size_t k = 0; k < n; ++k) {
                state = state * 6364136223846793005ull + 1442695040888963407ull;
                bytes.push_back(static_cast<char>(state >> 56));
            }
            QVERIFY(!code_of([&] { (void)pc::read(bytes, "x"); }).empty());
            std::string text;
            for (const char c : bytes.substr(0, 300)) text.push_back(static_cast<char>(0x20 + (static_cast<unsigned char>(c) % 0x5f)));
            QVERIFY(code_of([&] { (void)pc::read(bundle_text(text), "x"); }) != "other");
        }
    }

    // PC5 J-0 (render/pcraft_production): a document's keys Genko has no place for yet said as PC4 F-f said them — in its
    // order (レイヤーカンプ・注釈・スライス・タイムライン・変数とデータセット・計測), only those that hold something (a scale alone,
    // every document's, not), nothing written.
    void productionKeysSaidInTheirOrder() {
        namespace pp = genko::render::pcraft_production;
        const Json document = Json::parse(R"({"measurement": {"scale": {"pixel_length": 1}, "count_groups": [{"points": [[1, 2]]}]},
            "variables": {"name": {"kind": "text"}}, "timeline": {"fps": 12}, "slices": [], "notes": [{"text": "a"}],
            "layer_comps": [{"name": "c"}]})");
        std::vector<std::string> said;
        genko::core::Page page;
        pp::ReadContext context;
        context.page = &page;
        context.say = [&said](std::string note) { said.push_back(std::move(note)); };
        pp::read(document, context);
        // (PC5 J-a: レイヤーカンプ read — this one not of upstream's form: said so, in its place; PC5 I-d: 変数とデータセット likewise)
        QCOMPARE(said, (std::vector<std::string>{"レイヤーカンプは読めない形のため外しました", "注釈は Genko に無いため外しました",
                                                 "タイムラインは Genko に無いため外しました", "変数とデータセットは読めない形のため外しました",
                                                 "計測（カウント・ものさし）は Genko に無いため外しました"}));
        said.clear();
        pp::read(Json::parse(R"({"measurement": {"scale": {"pixel_length": 2}}})"), context);
        QVERIFY(said.empty());
        Json out = Json::object();
        pp::WriteContext writing;
        pp::write(page, out, writing);
        QVERIFY(out.empty());
    }
};

QTEST_GUILESS_MAIN(TestPcraft)
#include "test_pcraft.moc"
