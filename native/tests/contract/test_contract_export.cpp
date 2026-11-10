// The exports (formats/: export, the submission pack, the strip, PSD, EPUB, the dispatcher): what this build does not
// draw yet stops an export with nothing left behind; something in the way of a file, or a place that cannot be read,
// stops the whole export before any file moves; a pack stopped part way leaves nothing; the resolutions taken; a strip
// too large refused before its rows are made; a book still being read neither written nor checked.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>

#include <filesystem>
#include <functional>
#include <string>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

#include "core/base64.hpp"
#include "core/covers.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "export_files.hpp"
#include "formats/checks.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "render/brushes.hpp"
#include "render/colour.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/psd.hpp"
#include "render/psd_adjust.hpp"
#include "testsupport.hpp"
#include "storage/asset_store.hpp"
#include "storage/reader.hpp"
#include "storage/snapshot.hpp"

using genko::core::Json;
namespace formats = genko::formats;
namespace fs = std::filesystem;

namespace {

fs::path path_of(const QString& s) { return genko::core::path_from_utf8(s.toStdString()); }

// The most memory this process has held (kB) since the last reset (Linux: VmHWM; reset: from what it holds now through
// /proc/self/clear_refs); -1 where that cannot be read.
long long peak_kb(bool reset) {
#ifdef __linux__
    if (reset) {
        const int fd = ::open("/proc/self/clear_refs", O_WRONLY);
        if (fd < 0) return -1;
        const bool written = ::write(fd, "5", 1) == 1;
        ::close(fd);
        if (!written) return -1;
    }
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return -1;
    for (const QByteArray& line : status.readAll().split('\n')) {
        if (line.startsWith("VmHWM:")) return line.mid(6).trimmed().split(' ').front().toLongLong();
    }
#else
    (void)reset;
#endif
    return -1;
}

}  // namespace

class TestContractExport : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString books_;

private slots:
    void initTestCase() {
        QVERIFY(scratch_.isValid());
        books_ = scratch_.path() + "/books";
        // (a monochrome book of four pages)
        const genko::core::Document mono = genko::core::new_episode("モノクロ", genko::core::Num(1), 4, genko::core::PageSpec::a4_mono());
        genko::test::write_project(mono, path_of(books_ + "/mono.genko"));
    }

    // What this build does not draw yet stops an export: a placed picture on the last page of a book whose asset is a
    // JPEG 2000 file (this build cannot open one yet and says so, render::NotYetPorted "image_format") — and none of the
    // files of the pages before it is left behind.
    void notYetPorted() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        genko::core::Document doc = loaded.document;
        const QString assets = scratch_.path() + "/unported-assets";
        QVERIFY(QDir().mkpath(assets));
        doc.asset_dir = path_of(assets);
        const std::string planar = genko::core::a2b_base64("AAAADGpQICANCocKAAAAFGZ0eXBqcDIgAAAAAGpwMiA=");  // (JPEG 2000: Pillow opens it, this build does not)
        genko::core::Layer placed;
        placed.id = "placed000001";
        placed.kind = genko::core::LayerKind::Placed;
        placed.asset = genko::storage::AssetStore(path_of(assets)).put_bytes(planar, ".png");
        placed.placement_mm = genko::core::Rect{genko::core::Num(10), genko::core::Num(10), genko::core::Num(40), genko::core::Num(40)};
        doc.edit_page(doc.pages.size() - 1).layers.push_back(placed);
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/unported";
        const auto refused = [&](const char* what, const std::function<void(const fs::path&)>& make) {
            const QString dir = root + "/" + what;
            bool stopped = false;
            try {
                make(path_of(dir));
            } catch (const genko::render::NotYetPorted& e) {
                stopped = e.element() == "image_format";
            }
            QVERIFY2(stopped, what);
            QVERIFY2(genko::test::exports::tree(dir).empty(), what);  // (nothing written, not even the first pages)
        };
        refused("png", [&](const fs::path& d) { formats::export_print(doc, d, "png", 30); });
        refused("pdf", [&](const fs::path& d) { formats::export_print(doc, d, "pdf", 30); });
        refused("tiff", [&](const fs::path& d) { formats::export_print(doc, d, "tiff", 30); });
        refused("layers", [&](const fs::path& d) { formats::export_layers(doc, d, 30); });
        refused("psd", [&](const fs::path& d) { formats::export_page_psd(doc, *doc.pages.back(), d / "x.psd", 30); });
        refused("psd-pages", [&](const fs::path& d) { formats::export_psd_pages(doc, d, 30); });
        refused("pack", [&](const fs::path& d) { formats::export_pack(doc, d, "shueisha", 30); });
        refused("strip", [&](const fs::path& d) { formats::export_strip(doc, d / "s.png", 30); });
        refused("epub", [&](const fs::path& d) { formats::export_epub(doc, d / "b.epub", 30); });
        refused("webtoon", [&](const fs::path& d) { formats::export_webtoon(doc, d, 120, 100); });
        refused("sns", [&](const fs::path& d) { formats::export_sns(doc, d, 120); });
        // the dispatcher says so in its reply
        formats::RunOptions at30;
        at30.dpi = 30;
        const Json reply = formats::run(doc, std::nullopt, "png", path_of(root + "/run"), at30);
        QCOMPARE(reply.value("ok", true), false);
        QCOMPARE(QString::fromStdString(reply.value("code", std::string())), QStringLiteral("not_yet_ported"));
        QVERIFY(genko::test::exports::tree(root + "/run").empty());
        // and the official export (the studio's) is refused
        formats::RunOptions official;
        official.official = true;
        const Json studio = formats::run(doc, path_of(books_ + "/mono.genko"), "pdf", path_of(root + "/official"), official);
        QCOMPARE(QString::fromStdString(studio.value("code", std::string())), QStringLiteral("not_yet_ported"));
        QVERIFY(!QFileInfo::exists(root + "/official"));
    }

    // Something in the way of one of an export's files (a folder of its name): nothing is moved into place, the files
    // there before stay as they were.
    void somethingInTheWayStopsTheWholeExport() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        QVERIFY(doc.pages.size() >= 3);
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/in-the-way";
        const auto name = [&](std::size_t page, const char* suffix) {
            return QString::fromStdString(formats::stem(doc) + "_" + genko::core::file_stem(*doc.pages[page]) + suffix);
        };
        const QString png = root + "/png";
        QVERIFY(QDir().mkpath(png + "/" + name(1, ".png")));
        genko::test::write_bytes(png + "/" + name(0, ".png"), "an export before");
        QString said;
        try {
            formats::export_print(doc, path_of(png), "png", 30);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 21] Is a directory: '%1'").arg(png + "/" + name(1, ".png")));
        QCOMPARE(QString::fromStdString(genko::test::read_bytes(png + "/" + name(0, ".png"))), QStringLiteral("an export before"));
        QStringList there{name(0, ".png"), name(1, ".png")};
        there.sort();
        QCOMPARE(QDir(png).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name), there);
    }

    // A place whose state cannot be read (its folder cannot be looked into) stops the export before any file is moved
    // — not even the files elsewhere, set aside and put back.
    void anUnreadablePlaceStopsBeforeAnyFileMoves() {
        const QString open = scratch_.path() + "/unreadable/open";
        const QString shut = scratch_.path() + "/unreadable/shut";
        QVERIFY(QDir().mkpath(open) && QDir().mkpath(shut));
        genko::test::write_bytes(open + "/a.png", "an export before");
        QString said;
        bool untouched = false;
        {
            formats::detail::Output output;
            output.put(path_of(open + "/a.png"), "new a");
            output.put(path_of(shut + "/b.png"), "new b");
            QThread::msleep(30);  // (the folder's time from the files written into it, before the commit)
            const fs::file_time_type stamp = fs::last_write_time(path_of(open));
            fs::permissions(path_of(shut), fs::perms::owner_read | fs::perms::owner_write);  // (its names cannot be looked up)
            try {
                output.commit();
            } catch (const genko::core::Error& e) {
                said = QString::fromStdString(e.code() + ": " + e.what());
            }
            untouched = fs::last_write_time(path_of(open)) == stamp;  // (before the files not moved are removed)
            fs::permissions(path_of(shut), fs::perms::owner_all);
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 13] Permission denied: '%1'").arg(shut + "/b.png"));
        QCOMPARE(QString::fromStdString(genko::test::read_bytes(open + "/a.png")), QStringLiteral("an export before"));
        QVERIFY2(untouched, "a file was moved there and back");
    }

    // The submission pack is one export: its PNGs, which come after its TIFFs, stopped (a folder of a PNG's name) — no
    // TIFF left either.
    void aPackStoppedPartWayLeavesNothing() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString name = QString::fromStdString(formats::stem(doc) + "_" + genko::core::file_stem(*doc.pages[0]) + ".png");
        const QString pack = scratch_.path() + "/in-the-way-pack";
        QVERIFY(QDir().mkpath(pack + "/" + name));
        QString said;
        try {
            formats::export_pack(doc, path_of(pack), "shueisha", 30);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        QCOMPARE(said, QStringLiteral("io: [Errno 21] Is a directory: '%1'").arg(pack + "/" + name));
        QCOMPARE(QDir(pack).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList{name});
    }

    // The resolutions an export takes: 1 to 100000 dpi, refused past them before anything is drawn or written, also by
    // the dispatcher, whose dpi is a
    // 64-bit number (never cut to an int).
    void resolutions() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        const genko::core::Document& doc = loaded.document;
        genko::render::brushes::clear_custom();
        genko::render::brushes::register_book(doc.brush_custom);
        const QString root = scratch_.path() + "/resolutions";
        for (const std::string key : {"layers", "epub", "strip", "psd", "pack", "png"}) {
            for (const std::int64_t dpi : {std::int64_t{-5}, std::int64_t{100001}, std::int64_t{4294967446}}) {
                const QString dir = root + "/" + QString::fromStdString(key) + QString::number(dpi);
                formats::RunOptions o;
                o.dpi = dpi;
                Json reply;
                try {
                    reply = formats::run(doc, std::nullopt, key, path_of(dir), o);
                } catch (const std::exception& e) {
                    reply = Json{{"thrown", e.what()}};
                }
                QVERIFY2(reply == (Json{{"ok", false}, {"error", "the resolution must be between 1 and 100000 dpi"}}),
                         (key + " " + std::to_string(dpi) + ": " + reply.dump()).c_str());
                QVERIFY2(!QFileInfo::exists(dir), qPrintable(dir));
            }
        }
    }

    // A strip too large at its resolution (export_strip called with it: past what the dispatcher and `genko export`
    // take) is refused before its rows are made — not after holding memory for rows of 70 MB each.
    void aStripTooLargeIsRefusedBeforeItsRowsAreMade() {
        const QString root = scratch_.path() + "/strip-too-large";
        // (a book of three small pages: at ten million dpi each row of the strip would take 70 MB)
        const genko::core::Document small =
            genko::core::new_episode("s", genko::core::Num(1), 3, genko::core::PageSpec::custom(60, 60, 54, 54, 2, 5, 5, 4, 4, 600));
        const long long peak_before = peak_kb(true);
        QString said;
        try {
            formats::export_strip(small, path_of(root + "/s.png"), 10000000);
        } catch (const genko::core::Error& e) {
            said = QString::fromStdString(e.code() + ": " + e.what());
        }
        const long long peak_after = peak_kb(false);
        if (peak_before >= 0 && peak_after >= 0) {
            QVERIFY2(peak_after - peak_before < 200 * 1024, qPrintable(QStringLiteral("%1 kB more held while refused").arg(peak_after - peak_before)));
        }
        QCOMPARE(said, QStringLiteral("image_too_large: the strip is too large at this resolution"));
        QVERIFY(!QFileInfo::exists(root + "/s.png"));
    }

    // A book whose pages are still being read (the app's first page first, SPEC PERF-01): nothing is written from it or
    // checked in it — the pages not read yet would come out empty.
    void aBookStillBeingReadIsNeitherWrittenNorChecked() {
        const auto loaded = genko::storage::load_document(path_of(books_ + "/mono.genko"));
        genko::core::Document doc = loaded.document;
        doc.deferred.push_back(doc.pages.back());
        const QString dir = scratch_.path() + "/partial";
        const auto code_of = [](const std::function<void()>& make) {
            try {
                make();
            } catch (const genko::core::Error& e) {
                return std::string(e.code());
            }
            return std::string("done");
        };
        formats::RunOptions at30;
        at30.dpi = 30;
        QCOMPARE(code_of([&] { (void)formats::run(doc, std::nullopt, "png", path_of(dir), at30); }), std::string("page_not_loaded"));
        QVERIFY(!QFileInfo::exists(dir));
        QCOMPARE(code_of([&] { (void)formats::checks::book(doc); }), std::string("page_not_loaded"));
    }
};

QTEST_GUILESS_MAIN(TestContractExport)
#include "test_contract_export.moc"
