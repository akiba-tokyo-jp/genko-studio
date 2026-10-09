// import_psd (M4②b: Python's fileops.import_psd with psd.read_psd) against Python, on the PSD and PSB files of
// contract/psd_cases.json, which `pyref_harness.py make-psds` writes (RGB, grey, CMYK, bitmap, indexed, duotone and
// other modes; 1, 8, 16 and 32 bits; raw, PackBits, zip and zip with prediction; folders, hidden layers, masks,
// clipping, opacity, blend modes, text and adjustment layers, names, PSB; and broken ones):
//   1. readerLikePython: psd.read_psd of every file (render::psd::File here), and of each of a few files cut after
//      every byte: the same layers (names, folders and what is in them, opacity, visibility, blend, clipping, kind,
//      every picture and mask pixel for pixel), merged picture, size, dpi, mode and skipped layers, or the same error;
//      mutationsLikePython: the same for the files changed at a few bytes each (seeded), every one Python can be given
//      without gigabytes (see python_safe);
//   2. importLikePython: the op cases on the op book (`make-opsbook`), each step's reply, full snapshot and project.json
//      payload (the layers' pictures and masks compared by their pixels) as Python's, and the book saved and read back
//      as Python's is;
//   3. hostileFilesRefused: files Python would take gigabytes for (a layer of 40000 × 20000 PackBits rows of nothing,
//      a canvas of 100000 × 100000, a zip layer of nothing as wide as a house) refused without that memory, by the
//      reader and by the op, the book unchanged;
//   4. budgetsHold: the reader's limits (one channel, one layer, every channel together), shrunk, refuse a small file;
//   5. aWholeImportIsBounded: a small file of 32767 one-pixel layers on a canvas of 10000 × 12000 (Python makes a page
//      layer and a canvas-sized picture of each and keeps every PNG: hours, and gigabytes of PNGs) refused by the op
//      before any layer is made, at once, the book unchanged.
// Skipped without the Python reference.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <map>
#include <random>
#include <string>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "opsupport.hpp"
#include "render/ops_registry.hpp"
#include "render/psd.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/reader.hpp"
#include "testsupport.hpp"

using genko::core::Json;
namespace psd = genko::render::psd;

namespace {

Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    if (c.contains("dry")) step["dry_run"] = c["dry"];
    return Json::array({step});
}

// The harness's _image_digest: sha256 of "<mode>|<w>x<h>|" and the picture's bytes.
Json digest(const std::optional<genko::render::Image>& image) {
    if (!image) return nullptr;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const std::string head = std::string(image->mode()) + "|" + std::to_string(image->width()) + "x" + std::to_string(image->height()) + "|";
    hash.addData(QByteArrayView(head.data(), static_cast<qsizetype>(head.size())));
    const std::string bytes = image->tobytes();
    hash.addData(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())));
    return hash.result().toHex().toStdString();
}

// The harness's psd_reads record of `data`.
Json read_record(const std::string& data) {
    try {
        const psd::File file = psd::File::read(data);
        Json layers = Json::array();
        for (const psd::Layer& layer : file.layers()) {
            Json item = Json::object();
            item["name"] = layer.name;
            item["folder"] = layer.folder;
            item["parent"] = layer.parent ? Json(static_cast<std::int64_t>(*layer.parent)) : Json(nullptr);
            item["opacity"] = layer.opacity;
            item["visible"] = layer.visible;
            item["blend"] = layer.blend;
            item["clip"] = layer.clip;
            item["kind"] = layer.kind;
            item["image"] = layer.folder ? Json(nullptr) : digest(file.image(layer));
            item["mask"] = layer.folder ? Json(nullptr) : digest(file.mask(layer));
            layers.push_back(std::move(item));
        }
        Json out = Json::object();
        out["size"] = Json::array({file.width(), file.height()});
        out["dpi"] = file.dpi();
        out["mode"] = file.mode_name();
        Json skipped = Json::array();
        for (const std::string& name : file.skipped()) skipped.push_back(name);
        out["skipped"] = skipped;
        out["merged"] = digest(file.merged());
        out["layers"] = std::move(layers);
        return out;
    } catch (const psd::ReadError& error) {
        return Json::object({{"error", error.type()}, {"message", error.what()}});
    } catch (const std::exception& error) {  // (none: shown as a difference)
        return Json::object({{"error", "C++"}, {"message", error.what()}});
    }
}

// Whether Python can read `data` without taking gigabytes (it holds every channel decoded, pads PackBits rows to the
// layer's width as it goes, makes a picture over the whole canvas for every layer and decodes every plane of the
// merged picture): a canvas of at most 2 million pixels and 16 planes, and a read that this build, with its limits
// shrunk to 8 MB a channel, 2 million pixels a layer and 64 MB in all, does not refuse as too large.
bool python_safe(const std::string& data) {
    if (data.size() >= 26 && data.compare(0, 4, "8BPS") == 0) {
        const auto byte = [&](std::size_t i) { return static_cast<std::uint64_t>(static_cast<unsigned char>(data[i])); };
        const std::uint64_t planes = (byte(12) << 8) | byte(13);
        const std::uint64_t height = (byte(14) << 24) | (byte(15) << 16) | (byte(16) << 8) | byte(17);
        const std::uint64_t width = (byte(18) << 24) | (byte(19) << 16) | (byte(20) << 8) | byte(21);
        if (planes > 16 || width * height > 2'000'000) return false;
    }
    try {
        (void)psd::File::read(data, psd::Limits{8 << 20, 2'000'000, 64 << 20});
    } catch (const psd::ReadError& error) {
        return !error.too_large();
    }
    return true;
}

// An 8-bit RGB PSD of width × height holding `count` layers of one pixel each (raw red, green and blue samples) and
// nothing after them (no merged picture: read_psd has none).
std::string one_pixel_layers(std::uint32_t width, std::uint32_t height, int count) {
    const auto u16 = [](std::string& out, unsigned v) {
        out += {static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)};
    };
    const auto u32 = [](std::string& out, std::uint32_t v) {
        out += {static_cast<char>(v >> 24 & 0xFF), static_cast<char>(v >> 16 & 0xFF), static_cast<char>(v >> 8 & 0xFF),
                static_cast<char>(v & 0xFF)};
    };
    std::string info;  // the layer info: the count, the records, the channels' data
    u16(info, static_cast<unsigned>(count));
    for (int n = 0; n < count; ++n) {
        for (const std::uint32_t edge : {0U, 0U, 1U, 1U}) u32(info, edge);  // top, left, bottom, right
        u16(info, 3);
        for (unsigned channel = 0; channel < 3; ++channel) {
            u16(info, channel);
            u32(info, 3);  // (its compression, raw, and its one sample)
        }
        info += "8BIMnorm";
        info += {static_cast<char>(255), '\0', '\0', '\0'};  // opacity, clipping, flags, filler
        u32(info, 12);  // extra data: no mask, no blending ranges, an empty name padded to 4
        u32(info, 0);
        u32(info, 0);
        info.append(4, '\0');
    }
    for (int n = 0; n < 3 * count; ++n) info += std::string("\0\0\x80", 3);
    if (info.size() % 2 != 0) info += '\0';
    std::string out = "8BPS";
    u16(out, 1);
    out.append(6, '\0');
    u16(out, 3);  // channels
    u32(out, height);
    u32(out, width);
    u16(out, 8);  // bits
    u16(out, 3);  // RGB
    u32(out, 0);  // colour mode data
    u32(out, 0);  // image resources
    u32(out, static_cast<std::uint32_t>(4 + info.size() + 4));  // the layer and mask section
    u32(out, static_cast<std::uint32_t>(info.size()));
    out += info;
    u32(out, 0);  // global layer mask info
    return out;
}

}  // namespace

class TestContractPsd : public QObject {
    Q_OBJECT

    QTemporaryDir scratch_;
    QString book_;
    QString psds_;
    Json cases_;

    QString path(const QString& relative) const { return scratch_.path() + QLatin1Char('/') + relative; }
    QString file(const std::string& name) const { return psds_ + QLatin1Char('/') + QString::fromStdString(name) + QStringLiteral(".psd"); }

    // Every text of `value` with $PSD (the files' folder) and $B64:<name> (the file in base64) put in.
    Json placed(const Json& value) const {
        if (value.is_string()) {
            std::string text = value.get<std::string>();
            if (text.rfind("$B64:", 0) == 0) return genko::core::b64encode(genko::test::read_bytes(file(text.substr(5))));
            for (std::size_t at = text.find("$PSD"); at != std::string::npos; at = text.find("$PSD", at)) {
                text.replace(at, 4, psds_.toStdString());
            }
            return text;
        }
        Json out = value;
        if (value.is_array()) {
            for (std::size_t i = 0; i < value.size(); ++i) out[i] = placed(value[i]);
        } else if (value.is_object()) {
            for (const auto& [key, item] : value.items()) out[key] = placed(item);
        }
        return out;
    }

    genko::core::Document load_book() const {
        const genko::core::ScopedIdSource ids(genko::core::counting_ids());
        auto loaded = genko::storage::load_document(genko::storage::path_from_utf8(book_.toStdString()));
        if (!loaded.report.clean()) qFatal("%s", genko::core::dump_python(loaded.report.to_json()).c_str());
        return std::move(loaded.document);
    }

    // Each of `reads` ({"file", "cut"?}) read by psd.read_psd and here: a failure (QVERIFY) when a record differs.
    void compare_reads(const Json& reads, const QString& name, int least_read) {
        const QString dir = path(name);
        QDir().mkpath(dir);
        const Json jobs = Json::array({Json::object({{"op", "psd_reads"}, {"reads", reads}, {"out", (dir + "/py.json").toStdString()}})});
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, path("pyenv"), 1800000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(dir + "/py.json");
        QCOMPARE(records.size(), reads.size());
        std::vector<std::string> failures;
        std::map<std::string, int> outcomes;  // what Python's reads came to (each kind of error, or "read")
        int pictures = 0;
        for (std::size_t n = 0; n < reads.size(); ++n) {
            std::string data = genko::test::read_bytes(QString::fromStdString(reads[n]["file"].get<std::string>()));
            if (reads[n].contains("cut")) data.resize(reads[n]["cut"].get<std::size_t>());
            const Json got = read_record(data);
            const std::string want_text = genko::core::dump_python(records[n]);
            const std::string got_text = genko::core::dump_python(got);
            if (got_text != want_text) {
                failures.push_back(genko::core::dump_python(reads[n]) + ": " + got_text.substr(0, 700) + " != " + want_text.substr(0, 700));
                continue;
            }
            if (records[n].contains("error")) {
                outcomes[records[n]["error"].get<std::string>() + ": " + records[n]["message"].get<std::string>().substr(0, 40)] += 1;
            } else {
                outcomes["read"] += 1;
                pictures += static_cast<int>(records[n]["layers"].size()) + (records[n]["merged"].is_null() ? 0 : 1);
            }
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        for (const auto& [what, count] : outcomes) qInfo("%5d  %s", count, what.c_str());
        qInfo("%zu reads the same as Python's (%d layers and merged pictures among them)", reads.size() - failures.size(), pictures);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(reads.size()) + " reads differ").c_str());
        QVERIFY(outcomes["read"] >= least_read);
    }

private slots:
    void initTestCase() {
        if (genko::test::python_ref().isEmpty()) QSKIP("no reference Python: set GENKO_PYREF or install /opt/pyref/bin/python");
        QVERIFY(scratch_.isValid());
        book_ = path("ops.genko");
        psds_ = book_ + QStringLiteral("/psds");
        const QString cases = genko::test::repo_root() + QStringLiteral("/native/tests/contract/psd_cases.json");
        cases_ = genko::test::read_json(cases);
        const auto made = genko::test::harness({"make-opsbook", book_}, path("pyenv"), 1200000);
        QVERIFY2(made.finished && made.exit_code == 0, made.err.right(4000).constData());
        const auto written = genko::test::harness({"make-psds", cases, psds_}, path("pyenv"), 600000);
        QVERIFY2(written.finished && written.exit_code == 0, written.err.right(4000).constData());
    }

    void readerLikePython() {
        Json reads = Json::array();
        for (const Json& name : cases_["reads"]) reads.push_back(Json::object({{"file", file(name.get<std::string>()).toStdString()}}));
        for (const Json& name : cases_["cuts"]) {
            const std::string whole = genko::test::read_bytes(file(name.get<std::string>()));
            for (std::size_t cut = 0; cut < whole.size(); ++cut) {
                reads.push_back(Json::object({{"file", file(name.get<std::string>()).toStdString()}, {"cut", static_cast<std::int64_t>(cut)}}));
            }
        }
        compare_reads(reads, QStringLiteral("reads"), 100);
    }

    void mutationsLikePython() {
        const QString dir = path("mutations");
        QDir().mkpath(dir);
        std::mt19937 rng(20261009);
        Json reads = Json::array();
        int unsafe = 0;
        for (const Json& name : cases_["reads"]) {
            const std::string whole = genko::test::read_bytes(file(name.get<std::string>()));
            if (whole.empty() || whole.size() > 100000) continue;
            for (int k = 0; k < 40; ++k) {
                std::string changed = whole;
                const int changes = 1 + static_cast<int>(rng() % 4);
                for (int c = 0; c < changes; ++c) {
                    const std::size_t at = rng() % changed.size();
                    const unsigned how = rng() % 4;
                    const auto value = static_cast<unsigned char>(changed[at]);
                    changed[at] = static_cast<char>(how == 0 ? rng() & 0xFF : how == 1 ? value ^ (1u << (rng() % 8)) : how == 2 ? 0xFF : 0);
                }
                if (!python_safe(changed)) {
                    ++unsafe;
                    continue;
                }
                const QString changed_file = dir + QStringLiteral("/%1.psd").arg(reads.size());
                genko::test::write_bytes(changed_file, changed);
                reads.push_back(Json::object({{"file", changed_file.toStdString()}}));
            }
        }
        qInfo("%zu files changed at a few bytes (%d more not given to Python: they would take it gigabytes)", reads.size(), unsafe);
        compare_reads(reads, QStringLiteral("mutated"), 500);
    }

    void importLikePython() {
        const Json cases = placed(cases_["cases"]);
        const std::uint64_t first_id = cases_["first_id"].get<std::uint64_t>();
        const QString dir = path("ops");
        QDir().mkpath(dir);
        Json jobs = Json::array();
        for (std::size_t n = 0; n < cases.size(); ++n) {
            Json job = Json::object();
            job["op"] = "steps";
            job["book"] = book_.toStdString();
            job["ids"] = true;
            job["first_id"] = first_id;
            job["store"] = (dir + QStringLiteral("/py-store-%1").arg(n)).toStdString();
            job["steps"] = steps_of(cases[n]);
            job["out"] = (dir + QStringLiteral("/%1.json").arg(n)).toStdString();
            if (cases[n]["ok"].get<bool>()) job["reread"] = (dir + QStringLiteral("/py-%1.genko").arg(n)).toStdString();
            jobs.push_back(std::move(job));
        }
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, path("pyenv"), 3000000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());

        const genko::core::Document book = load_book();
        QVERIFY(book.asset_dir.has_value());
        int successes = 0;
        int refusals = 0;
        int read_back = 0;
        genko::test::ReadBackNotes notes;
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const bool expect_ok = c["ok"].get<bool>();
            Json records = genko::test::read_json(QString::fromStdString(jobs[n]["out"].get<std::string>()));
            Json reread;
            if (!records.empty() && records.back().contains("reread")) {
                reread = records.back();
                records.erase(records.size() - 1);
            }
            genko::storage::AssetStore store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-store-%1").arg(n)).toStdString()));
            genko::core::Document last;
            const auto outcomes = genko::test::run_steps(book, steps_of(c), first_id, store, false, &last);
            if (records.size() != outcomes.size()) {
                failures.push_back(name + ": Python gave " + std::to_string(records.size()) + " steps");
                continue;
            }
            const Json& last_reply = records.back()["reply"];
            if (last_reply["ok"].get<bool>() != expect_ok) {
                failures.push_back(name + ": Python gave " + genko::core::dump_python(last_reply).substr(0, 400));
                continue;
            }
            bool same = true;
            for (std::size_t s = 0; s < outcomes.size() && same; ++s) {
                const std::string diff = genko::test::compare_step(outcomes[s], records[s]);
                if (!diff.empty()) {
                    failures.push_back(name + " step " + std::to_string(s) + ": " + diff.substr(0, 1500));
                    same = false;
                }
            }
            if (!same) continue;
            (expect_ok ? successes : refusals) += 1;
            if (!expect_ok) continue;
            const std::string difference = genko::test::read_back_difference(
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes);
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        qInfo("import_psd: %d successes and %d refusals the same as Python; saved and read back as they were: %d", successes,
              refusals, read_back);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        QVERIFY(successes >= 70 && refusals >= 60 && read_back == successes);
    }

    void hostileFilesRefused() {
        const genko::core::Document book = load_book();
        const genko::core::CommandBus bus(genko::render::ops_registry());
        for (const Json& c : cases_["cpp_only"]) {
            const std::string name = c["n"].get<std::string>();
            const std::string data = genko::test::read_bytes(file(c["file"].get<std::string>()));
            if (c.contains("says")) {
                try {
                    (void)psd::File::read(data);
                    QFAIL((name + ": read").c_str());
                } catch (const psd::ReadError& error) {
                    QVERIFY2(error.too_large() && std::string(error.what()) == c["says"].get<std::string>(), (name + ": " + error.what()).c_str());
                }
            }
            const Json ops = Json::array({Json::object({{"op", "import_psd"}, {"page", 6}, {"path", file(c["file"].get<std::string>()).toStdString()}})});
            const std::string says = c.contains("op_says") ? c["op_says"].get<std::string>()
                                                           : "the PSD cannot be read (" + c["says"].get<std::string>() + ")";
            try {
                (void)bus.apply(book, ops, genko::core::Actor("genko"));
                QFAIL((name + ": imported").c_str());
            } catch (const genko::core::ApplyError& error) {
                QVERIFY2(error.code() == "apply" && std::string(error.what()).find("ops[0] import_psd: " + says) == 0,
                         (name + ": " + error.what()).c_str());
            }
        }
    }

    void budgetsHold() {
        const std::string data = genko::test::read_bytes(file("rgb8"));
        const psd::Limits roomy = psd::default_limits();
        QVERIFY(!psd::File::read(data, roomy).layers().empty());
        const auto refused = [&](psd::Limits limits, const std::string& says) {
            try {
                (void)psd::File::read(data, limits);
            } catch (const psd::ReadError& error) {
                return error.too_large() && error.what() == says;
            }
            return false;
        };
        psd::Limits limits = roomy;
        limits.max_channel_bytes = 200;  // (a channel of 16 × 12 is 192 bytes, of 17 × 14 is 238)
        QVERIFY(refused(limits, "a layer is too large to read"));
        limits = roomy;
        limits.max_layer_pixels = 200;
        QVERIFY(refused(limits, "a layer is too large to read"));
        limits = roomy;
        limits.max_decoded_bytes = 2000;
        QVERIFY(refused(limits, "the layers are too large to read"));
        // a zip stream's whole output counts, past what the channel keeps
        const std::string zip = genko::test::read_bytes(file("zip-long"));
        limits = roomy;
        limits.max_decoded_bytes = 48 + 100 - 1;  // (its alpha of 8 × 6, then a zip stream of 100 bytes)
        try {
            (void)psd::File::read(zip, limits);
            QFAIL("read with a budget smaller than its zip's output");
        } catch (const psd::ReadError& error) {
            QVERIFY(error.too_large());
        }
        limits.max_decoded_bytes = 48 + 100;
        QVERIFY(!psd::File::read(zip, limits).layers().empty());
    }

    // The op as a whole: each pixel layer is a page layer and a picture as large as the canvas, made, drawn and kept as
    // a PNG. 32767 layers of one pixel on the largest canvas (2.4 MB of file) would be hours of that and gigabytes of
    // PNGs: refused before any layer is made (a refusal of this build's; Python fails, later, with MemoryError).
    void aWholeImportIsBounded() {
        const genko::core::Document book = load_book();
        const auto pages = book.pages;
        const std::size_t layers_before = book.page(5).layers.size();
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const std::string data = one_pixel_layers(10000, 12000, 32767);
        QVERIFY(data.size() < 2'500'000);
        QCOMPARE(psd::File::read(data).layers().size(), std::size_t{32767});  // (the reader takes it: a pixel a layer)
        const auto import = [&](const std::string& file) {
            const Json op = Json::object({{"op", "import_psd"}, {"page", 6}, {"psd", genko::core::b64encode(file)}});
            return Json::array({op});
        };
        QElapsedTimer clock;
        clock.start();
        try {
            (void)bus.apply(book, import(data), genko::core::Actor("genko"));
            QFAIL("imported");
        } catch (const genko::core::ApplyError& error) {
            const std::string says =
                "ops[0] import_psd: the PSD has too many layers for their size (32767 layers, each a page layer of ";
            QVERIFY2(error.code() == "apply" && std::string(error.what()).starts_with(says), error.what());
        }
        QVERIFY2(clock.elapsed() < 20000, std::to_string(clock.elapsed()).c_str());
        QVERIFY(book.pages == pages);
        QCOMPARE(book.page(5).layers.size(), layers_before);
        // many layers on a canvas of a page's size are taken
        const auto taken = bus.apply(book, import(one_pixel_layers(1000, 1400, 60)), genko::core::Actor("genko"));
        QCOMPARE(taken.doc.page(5).layers.size(), layers_before + 60);
    }
};

QTEST_GUILESS_MAIN(TestContractPsd)
#include "test_contract_psd.moc"
