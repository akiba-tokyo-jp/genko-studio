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
//      before any layer is made, at once, the book unchanged;
//   6. pictureLikePillow: a PSD opened as one picture (render::open_image, render/psd_image.hpp) as Pillow 12.3's
//      Image.open and load() open it, on every file (the "pil" ones are made for this: each mode, raw and PackBits,
//      odd sizes, palettes, too few channels, a bomb, PackBits and raw data cut short or running over, sections that
//      say more than the file has) and on a few files cut after every byte: the same mode, size and pixels, and the
//      same pixels made RGBA, or the same error at the same stage (open or load); a Lab file decoded the same, though
//      open_image refuses it as not yet ported (Pillow turns Lab into RGB through Little CMS); pictureMutationsLikePillow:
//      the same for the files changed at a few bytes each (seeded).
// The user's decision 5 (PC2a, SPEC.md COLOR-04): this build reads an adjustment layer a codec reads (render/psd_adjust)
// as a correction layer, where psd.read_psd leaves every one out. The reads are compared with Python's with those
// layers taken out — Python's skipped this build's with exactly their names among them —, and import_psd's cases as
// Python reads the files (psd::PythonView: no codec reads), then again as this build reads them: the same but the
// correction layers made of the adjustment layers Python leaves out.
// The user's decision 3 (a mask's settings always read; the coordinator after the PC3 review): this build reads a
// layer's mask that is off (flags bit 1), which psd.read_psd leaves out. The reads are compared with such a mask left
// out; the import cases of a file with one are marked "cpp": "deviates" (Python's but at that layer's mask); and
// aMaskThatIsOffIsRead checks it is read and imported off.
// Skipped without the Python reference.

#include <QtTest>

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <map>
#include <set>
#include <optional>
#include <random>
#include <string>

#include "core/base64.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/paths.hpp"
#include "core/psd_keep.hpp"
#include "formats/copy.hpp"
#include "formats/export.hpp"
#include "formats/open_file.hpp"
#include "opsupport.hpp"
#include "render/not_yet_ported.hpp"
#include "render/ops_registry.hpp"
#include "render/png.hpp"
#include "render/psd.hpp"
#include "render/psd_adjust.hpp"
#include "render/psd_image.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"
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

// The harness's psd_reads record of `data`. (The user's decision 5: the adjustment layers read as correction layers
// are taken out of "layers" — a folder's index counted without them — and named in "adjusted", this build's own.)
Json read_record(const std::string& data) {
    try {
        const psd::File file = psd::File::read(data);
        std::vector<std::int64_t> index(file.layers().size(), -1);
        std::int64_t kept = 0;
        for (std::size_t i = 0; i < file.layers().size(); ++i)
            if (!file.layers()[i].adjust) index[i] = kept++;
        Json adjusted = Json::array();
        Json layers = Json::array();
        for (const psd::Layer& layer : file.layers()) {
            if (layer.adjust) {
                adjusted.push_back(layer.name);
                continue;
            }
            Json item = Json::object();
            item["name"] = layer.name;
            item["folder"] = layer.folder;
            item["parent"] = layer.parent ? Json(index[*layer.parent]) : Json(nullptr);
            item["opacity"] = layer.opacity;
            item["visible"] = layer.visible;
            item["blend"] = layer.blend;
            item["clip"] = layer.clip;
            item["kind"] = layer.kind;
            item["image"] = layer.folder ? Json(nullptr) : digest(file.image(layer));
            // (a mask that is off: read too here — the user's decision 3, aMaskThatIsOffIsRead —, which psd.read_psd
            // leaves out; compared as Python reads the file)
            item["mask"] = layer.folder || !layer.mask_enabled ? Json(nullptr) : digest(file.mask(layer));
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
        if (!adjusted.empty()) out["adjusted"] = std::move(adjusted);
        return out;
    } catch (const psd::ReadError& error) {
        return Json::object({{"error", error.type()}, {"message", error.what()}});
    } catch (const std::exception& error) {  // (none: shown as a difference)
        return Json::object({{"error", "C++"}, {"message", error.what()}});
    }
}

// "" when `cpp`'s skipped layers are `python`'s but the names of the correction layers this build read ("adjusted",
// the user's decision 5): this build's in their order among Python's, and the rest of Python's exactly those names.
std::string skipped_but_adjusted(const Json& cpp, const Json& python) {
    if (!python.contains("skipped") || !python["skipped"].is_array()) return "Python did not read it";
    const Json& all = python["skipped"];
    std::size_t at = 0;
    std::multiset<std::string> rest;
    for (const Json& name : cpp["skipped"]) {
        while (at < all.size() && all[at] != name) rest.insert(all[at++].get<std::string>());
        if (at == all.size()) return "a skipped layer that Python did not skip: " + name.dump();
        ++at;
    }
    while (at < all.size()) rest.insert(all[at++].get<std::string>());
    std::multiset<std::string> read;
    for (const Json& name : cpp["adjusted"]) read.insert(name.get<std::string>());
    if (rest != read) return "the correction layers read are not the layers Python left out but this build's";
    return {};
}

// "same" when `own` (import_psd's reply as this build reads the file) is `python_view`'s (as Python reads it); "" when
// it differs only by correction layers made of adjustment layers (the user's decision 5): each import's layers
// Python's view's with those among them (their titles, the layers kind adjust in `after`), its skipped layers the
// view's without exactly those names; else what differs.
std::string corrections_more(const Json& own, const Json& python_view, const genko::core::Document& after) {
    if (genko::core::dump_python(own) == genko::core::dump_python(python_view)) return "same";
    if (!own.value("ok", false) || !python_view.value("ok", false) || !own.contains("results") || !python_view.contains("results") ||
        own["results"].size() != python_view["results"].size())
        return "the replies differ: " + genko::core::dump_python(own).substr(0, 600);
    std::multiset<std::string> made;  // (the titles of the correction layers in the book after)
    for (const auto& page : after.pages)
        for (const auto& layer : page->layers)
            if (layer.kind == genko::core::LayerKind::Adjust) made.insert(layer.title);
    int more = 0;
    for (std::size_t r = 0; r < own["results"].size(); ++r) {
        const Json& mine = own["results"][r];
        const Json& theirs = python_view["results"][r];
        if (!mine.contains("layers") || !theirs.contains("layers")) continue;
        // their layers' titles in order among mine; the others correction layers
        std::size_t at = 0;
        std::multiset<std::string> extra;
        for (const Json& layer : mine["layers"]) {
            if (at < theirs["layers"].size() && layer["title"] == theirs["layers"][at]["title"]) {
                ++at;
                continue;
            }
            extra.insert(layer["title"].get<std::string>());
        }
        if (at != theirs["layers"].size()) return "a layer Python's view made is not made";
        for (const std::string& title : extra)
            if (made.count(title) == 0) return "a layer more that is not a correction layer: " + title;
        // their skipped: mine with exactly those titles among them
        Json both = Json::object({{"skipped", mine.value("skipped", Json::array())}, {"adjusted", Json::array()}});
        for (const std::string& title : extra) both["adjusted"].push_back(title);
        const std::string why = skipped_but_adjusted(both, Json::object({{"skipped", theirs.value("skipped", Json::array())}}));
        if (!why.empty()) return why;
        more += static_cast<int>(extra.size());
    }
    return more > 0 ? std::string() : "the replies differ: " + genko::core::dump_python(own).substr(0, 600);
}

// The harness's psd_images record of `data` (Pillow's Image.open, then load()): what render::psd_image_size refuses
// is Image.open's error, what render::open_image refuses then is load()'s. A Lab file opens as Pillow's "LAB" picture
// (color-mode@1, SPEC.md COLOR-02); its RGBA (Little CMS's, another version on each side: within one, as
// test_contract_ink compares it) is not compared here: "rgba" is "(Lab)".
Json image_record(const std::string& data) {
    const auto refused = [](const genko::core::Error& error, const char* stage) {
        static const std::map<std::string, std::string> types{
            {"unidentified_image", "UnidentifiedImageError"}, {"format", "OSError"}, {"image_too_large", "DecompressionBombError"}};
        const auto type = types.find(error.code());
        Json out = Json::object();
        out["error"] = type == types.end() ? "C++ " + error.code() : type->second;
        out["message"] = error.code() == "unidentified_image" ? std::string("cannot identify image file <_io.BytesIO object>") : std::string(error.what());
        out["stage"] = stage;
        return out;
    };
    try {
        (void)genko::render::psd_image_size(data, genko::render::kPillowOpenLimits);
    } catch (const genko::render::NotYetPorted& error) {
        return Json::object({{"error", "C++"}, {"message", error.what()}});
    } catch (const genko::core::Error& error) {
        return refused(error, "open");
    }
    const bool lab = genko::render::psd_lab(data);
    std::optional<genko::render::Image> image;
    try {
        image = genko::render::open_image(data, genko::render::kPillowOpenLimits);
    } catch (const genko::render::NotYetPorted& error) {
        return Json::object({{"error", "C++"}, {"message", error.what()}});
    } catch (const genko::core::Error& error) {
        return refused(error, "load");
    }
    Json out = Json::object();
    out["format"] = "PSD";
    out["mode"] = std::string(image->mode());
    out["size"] = Json::array({image->width(), image->height()});
    out["image"] = digest(image);
    if (lab) {
        out["rgba"] = "(Lab)";
    } else {
        try {
            out["rgba"] = digest(image->convert("RGBA"));
        } catch (const genko::core::Error& error) {
            out["rgba"] = Json::array({"C++ " + error.code(), std::string(error.what())});
        }
    }
    return out;
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

// (PC4 F-d) Big-endian numbers.
void be16(std::string& out, unsigned v) { out += {static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)}; }
void be32(std::string& out, std::uint32_t v) {
    out += {static_cast<char>(v >> 24 & 0xFF), static_cast<char>(v >> 16 & 0xFF), static_cast<char>(v >> 8 & 0xFF), static_cast<char>(v & 0xFF)};
}

// (PC4 F-d) An RGB PSD of width × height at `depth` with one layer of one pixel (raw samples) whose record holds `blocks`
// (tagged blocks as they are) after its name, and no merged picture.
std::string psd_with_blocks(const std::string& blocks, std::uint32_t width, std::uint32_t height, int depth = 8) {
    const unsigned sample = static_cast<unsigned>(depth / 8);
    std::string info;
    be16(info, 1);
    for (const std::uint32_t edge : {0U, 0U, 1U, 1U}) be32(info, edge);
    be16(info, 4);
    for (const int channel : {-1, 0, 1, 2}) {
        be16(info, static_cast<unsigned>(static_cast<std::int16_t>(channel)) & 0xFFFF);
        be32(info, 2 + sample);
    }
    info += "8BIMnorm";
    info += {static_cast<char>(255), '\0', '\0', '\0'};
    std::string extra;
    be32(extra, 0);  // (no mask)
    be32(extra, 0);  // (no blending ranges)
    extra += std::string("\x01x\0\0", 4);  // (its name "x", padded)
    extra += blocks;
    be32(info, static_cast<std::uint32_t>(extra.size()));
    info += extra;
    for (int c = 0; c < 4; ++c) info += std::string(2, '\0') + std::string(sample, '\x7f');
    if (info.size() % 2) info += '\0';
    std::string out = "8BPS";
    be16(out, 1);
    out.append(6, '\0');
    be16(out, 3);
    be32(out, height);
    be32(out, width);
    be16(out, static_cast<unsigned>(depth));
    be16(out, 3);
    be32(out, 0);
    be32(out, 0);
    be32(out, static_cast<std::uint32_t>(4 + info.size() + 4));
    be32(out, static_cast<std::uint32_t>(info.size()));
    out += info;
    be32(out, 0);
    return out;
}

// (PC4 F-d) An 8-bit RGB PSD of width × height of no layers (its layer info's length 0), `global` after its global layer
// mask info, and a raw merged picture.
std::string flat_with_global(const std::string& global, std::uint32_t width, std::uint32_t height) {
    std::string out = "8BPS";
    be16(out, 1);
    out.append(6, '\0');
    be16(out, 3);
    be32(out, height);
    be32(out, width);
    be16(out, 8);
    be16(out, 3);
    be32(out, 0);
    be32(out, 0);
    be32(out, static_cast<std::uint32_t>(8 + global.size()));
    be32(out, 0);
    be32(out, 0);
    out += global;
    be16(out, 0);
    out += std::string(std::size_t(width) * height * 3, '\x40');
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

    // (PC4 F-d) the fixed files of psd-tools (native/tests/data/psd_keep: make_psd_keep.py, MANIFEST.json), their bytes
    // checked against the manifest
    static std::string keep_path(const std::string& name) {
        return (genko::test::repo_root() + QStringLiteral("/native/tests/data/psd_keep/") + QString::fromStdString(name)).toStdString();
    }
    static std::string keep_file(const std::string& name) {
        const std::string bytes = genko::test::read_bytes(QString::fromStdString(keep_path(name)));
        const Json manifest = genko::test::read_json(QString::fromStdString(keep_path("MANIFEST.json")));
        const QByteArray sha = QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex();
        if (!manifest["files"].contains(name) || manifest["files"][name].get<std::string>() != sha.toStdString()) qFatal("%s is not the file of MANIFEST.json", name.c_str());
        return bytes;
    }
    // (PC4 F-d) the files as psd-tools reads them (tools/migration/psd_keep_check.py: {file: its view}); {"error"} when it
    // cannot
    Json tools_view(const QStringList& files, bool samples = false) {
        const QString out = path(QStringLiteral("psd-tools-%1.json").arg(++views_));
        QStringList args{genko::test::repo_root() + QStringLiteral("/tools/migration/psd_keep_check.py"), out};
        if (samples) args << QStringLiteral("--samples");
        args << files;
        const auto ran = genko::test::run(genko::test::python_ref(), args, genko::test::python_env(path(QStringLiteral("pyenv"))));
        if (ran.exit_code != 0) return Json::object({{"error", ran.err.toStdString()}});
        return genko::test::read_json(out);
    }
    int views_ = 0;

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
        int adjusted = 0;  // (reads with adjustment layers read as correction layers: the user's decision 5)
        for (std::size_t n = 0; n < reads.size(); ++n) {
            std::string data = genko::test::read_bytes(QString::fromStdString(reads[n]["file"].get<std::string>()));
            if (reads[n].contains("cut")) data.resize(reads[n]["cut"].get<std::size_t>());
            Json got = read_record(data);
            if (got.contains("adjusted")) {
                const std::string why = skipped_but_adjusted(got, records[n]);
                if (!why.empty()) {
                    failures.push_back(genko::core::dump_python(reads[n]) + ": " + why + ": " + genko::core::dump_python(got).substr(0, 700) +
                                       " != " + genko::core::dump_python(records[n]).substr(0, 700));
                    continue;
                }
                got.erase("adjusted");
                got["skipped"] = records[n]["skipped"];
                ++adjusted;
            }
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
        qInfo("%zu reads the same as Python's (%d layers and merged pictures among them; %d with adjustment layers read as "
              "correction layers, the user's decision 5)", reads.size() - failures.size(), pictures, adjusted);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(reads.size()) + " reads differ").c_str());
        QVERIFY(outcomes["read"] >= least_read);
    }

    // Each of `reads` ({"file", "cut"?}) opened as one picture by Pillow and here: a failure (QVERIFY) when a record
    // differs.
    void compare_pictures(const Json& reads, const QString& name, int least_read) {
        const QString dir = path(name);
        QDir().mkpath(dir);
        const Json jobs = Json::array({Json::object({{"op", "psd_images"}, {"reads", reads}, {"out", (dir + "/py.json").toStdString()}})});
        genko::test::write_bytes(dir + "/jobs.json", genko::core::dump_python(jobs));
        const auto ran = genko::test::harness({"batch", dir + "/jobs.json"}, path("pyenv"), 1800000);
        QVERIFY2(ran.finished && ran.exit_code == 0, ran.err.right(4000).constData());
        const Json records = genko::test::read_json(dir + "/py.json");
        QCOMPARE(records.size(), reads.size());
        std::vector<std::string> failures;
        std::map<std::string, int> outcomes;  // what Pillow made of them (each kind of error, or each mode read)
        int lab = 0;
        for (std::size_t n = 0; n < reads.size(); ++n) {
            std::string data = genko::test::read_bytes(QString::fromStdString(reads[n]["file"].get<std::string>()));
            if (reads[n].contains("cut")) data.resize(reads[n]["cut"].get<std::size_t>());
            Json got = image_record(data);
            if (got.contains("rgba") && got["rgba"] == Json("(Lab)") && records[n].contains("rgba")) {
                got["rgba"] = records[n]["rgba"];
                ++lab;
            }
            const std::string want_text = genko::core::dump_python(records[n]);
            const std::string got_text = genko::core::dump_python(got);
            if (got_text != want_text) {
                failures.push_back(genko::core::dump_python(reads[n]) + ": " + got_text.substr(0, 700) + " != " + want_text.substr(0, 700));
                continue;
            }
            if (records[n].contains("error")) {
                outcomes[records[n]["stage"].get<std::string>() + " " + records[n]["error"].get<std::string>() + ": " +
                         records[n]["message"].get<std::string>().substr(0, 50)] += 1;
            } else {
                outcomes["read " + records[n]["mode"].get<std::string>()] += 1;
            }
        }
        for (std::size_t i = 0; i < failures.size() && i < 60; ++i) qWarning("%s", failures[i].c_str());
        int read = 0;
        for (const auto& [what, count] : outcomes) {
            qInfo("%5d  %s", count, what.c_str());
            if (what.starts_with("read ")) read += count;
        }
        qInfo("%zu pictures the same as Pillow's (%d of Lab, their RGBA not compared)", reads.size() - failures.size(), lab);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(reads.size()) + " pictures differ").c_str());
        QVERIFY(read >= least_read);
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

    void pictureLikePillow() {
        Json reads = Json::array();
        for (const char* list : {"pil", "reads"}) {
            for (const Json& name : cases_[list]) reads.push_back(Json::object({{"file", file(name.get<std::string>()).toStdString()}}));
        }
        for (const char* list : {"pil_cuts", "cuts"}) {
            for (const Json& name : cases_[list]) {
                const std::string whole = genko::test::read_bytes(file(name.get<std::string>()));
                for (std::size_t cut = 0; cut < whole.size(); ++cut) {
                    reads.push_back(Json::object({{"file", file(name.get<std::string>()).toStdString()}, {"cut", static_cast<std::int64_t>(cut)}}));
                }
            }
        }
        compare_pictures(reads, QStringLiteral("pictures"), 40);
    }

    void pictureMutationsLikePillow() {
        const QString dir = path("picture-mutations");
        QDir().mkpath(dir);
        std::mt19937 rng(20261011);
        Json reads = Json::array();
        int unsafe = 0;
        for (const char* list : {"pil", "reads"}) {
            for (const Json& name : cases_[list]) {
                const std::string whole = genko::test::read_bytes(file(name.get<std::string>()));
                if (whole.empty() || whole.size() > 100000) continue;
                for (int k = 0; k < 25; ++k) {
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
        }
        qInfo("%zu files changed at a few bytes (%d more not given to Python: they would take it gigabytes)", reads.size(), unsafe);
        compare_pictures(reads, QStringLiteral("picture-mutated"), 300);
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
        int corrected = 0;
        int deviating = 0;
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
            std::vector<genko::test::StepOutcome> outcomes;
            {
                const psd::PythonView python_view;  // (as psd.read_psd reads the file: every adjustment layer left out)
                outcomes = genko::test::run_steps(book, steps_of(c), first_id, store, false, &last);
            }
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
            // (a case marked "cpp": "deviates" — the user's decision 3: a mask that is off read, which Python leaves
            // out — is Python's but at its places, and differs there)
            const bool deviates = genko::test::deviates(c);
            if (deviates) {
                if (const std::string diff = genko::test::compare_deviating_steps(outcomes, records, c); !diff.empty()) {
                    failures.push_back(name + ": " + diff.substr(0, 1500));
                    same = false;
                }
                ++deviating;
            }
            for (std::size_t s = 0; s < outcomes.size() && same && !deviates; ++s) {
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
                last, genko::storage::path_from_utf8((dir + QStringLiteral("/cpp-%1.genko").arg(n)).toStdString()), store, reread, notes,
                genko::test::deviation_paths(c));
            if (!difference.empty()) {
                failures.push_back(name + ": " + difference);
                continue;
            }
            ++read_back;
            // as this build reads the file (the user's decision 5): the same, or the correction layers made of the
            // adjustment layers Python leaves out more
            genko::storage::AssetStore own_store(genko::storage::path_from_utf8((dir + QStringLiteral("/cpp5-store-%1").arg(n)).toStdString()));
            genko::core::Document own_last;
            const auto own = genko::test::run_steps(book, steps_of(c), first_id, own_store, false, &own_last);
            const std::string why = corrections_more(own.back().reply, outcomes.back().reply, own_last);
            if (why == "same") continue;
            if (!why.empty()) {
                failures.push_back(name + " (as this build reads it): " + why);
                continue;
            }
            ++corrected;
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        qInfo("import_psd: %d successes and %d refusals the same as Python; saved and read back as they were: %d; %d with "
              "correction layers made of adjustment layers (the user's decision 5); %d deviating on purpose (the user's decision "
              "3: a mask that is off read)",
              successes, refusals, read_back, corrected, deviating);
        QVERIFY(deviating == 2);
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) +
                                    " cases differ from Python (see the warnings)").c_str());
        QVERIFY(successes >= 70 && refusals >= 60 && read_back == successes);
    }

    // The user's decision 3 (a mask's settings always read): a layer's mask that is off (flags bit 1), which psd.read_psd
    // leaves out, read with its picture and kept off — not in the picture of the layers laid together —, and imported
    // as a mask that is off.
    void aMaskThatIsOffIsRead() {
        const std::string data = genko::test::read_bytes(file("masks"));
        const psd::File read = psd::File::read(data);
        const auto off = std::find_if(read.layers().begin(), read.layers().end(), [](const psd::Layer& l) { return l.name == "disabled"; });
        QVERIFY(off != read.layers().end());
        QVERIFY(off->has_mask && !off->mask_enabled);
        const std::optional<genko::render::Image> picture = read.mask(*off);
        QVERIFY(picture.has_value());
        QVERIFY(picture->getbbox().has_value());  // (its box [4, 4, 14, 12] white: the default black around it)
        for (const psd::Layer& l : read.layers())
            if (l.name != "disabled" && l.has_mask) QVERIFY(l.mask_enabled);
        const genko::core::Document book = load_book();
        const genko::core::Document done =
            genko::core::CommandBus(genko::render::ops_registry())
                .apply(book, Json::array({Json{{"op", "import_psd"}, {"page", 6}, {"path", file("masks").toStdString()}, {"id", "off"}}}),
                       genko::core::Actor("genko"))
                .doc;
        int seen = 0;
        for (const auto& page : done.pages) {
            for (const auto& layer : page->layers) {
                if (layer.title != "disabled") continue;
                ++seen;
                QVERIFY(layer.mask && !layer.mask->enabled && layer.mask->png && !layer.mask->png->empty());
            }
        }
        QCOMPARE(seen, 1);
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

    // --- PC4 F-d (SPEC.md FILE-01「PSD・PSB」): psd-keep@1, the layers at their depth, PSB ------------------------------

    // psd-keep@1: a PSD of psd-tools (native/tests/data/psd_keep/keep.psd: layer effects, a label colour, locks, Blend If,
    // layer ids, fill opacity, an effects' reference point; layer comps, slices, the global light, a URL list; notes and a
    // pattern no fill uses) opened as a book and written again: every block and resource this build does not read comes
    // back byte for byte — psd-tools reads the same blocks and resources of each layer and of the file, this build's
    // reader the same payloads —, the locks copied to the layers and written back, the layer ids written; the book saved
    // and read back keeps them (its assets, psd-keep@1), and writes the same file; the effects said (the report, the
    // notes); a layer deleted drops the comps and slices that name it, a merge drops what the merged layer kept, a
    // duplicate keeps it (its layer id written once).
    void keptBlocksComeBack() {
        namespace core = genko::core;
        namespace fs = std::filesystem;
        const std::string source = keep_file("keep.psd");
        genko::formats::Opened opened = genko::formats::open_file(source, "keep.psd");
        core::Document& doc = opened.doc;
        const core::Page& page = doc.page(0);
        QVERIFY(page.native_psd.has_value());
        QCOMPARE(page.native_psd->from, (std::optional<core::PsdFrom>(core::PsdFrom{1, 3, 8})));
        std::set<int> resources;
        for (const core::PsdResource& r : page.native_psd->resources) resources.insert(r.id);
        QCOMPARE(resources, (std::set<int>{1037, 1049, 1050, 1054, 1065}));  // (not 1005, 1057, 1060: this build's own)
        std::set<std::string> globals;
        for (const core::PsdGlobal& g : page.native_psd->globals) globals.insert(g.key);
        QCOMPARE(globals, (std::set<std::string>{"Anno", "Patt"}));
        QCOMPARE(page.native_psd->ids, (std::vector<std::int64_t>{7, 8, 9}));
        const auto layer_named = [](const core::Page& on, const std::string& title) -> const core::Layer* {
            for (const core::Layer& l : on.layers)
                if (l.title == title) return &l;
            return nullptr;
        };
        const core::Layer* effects = layer_named(page, "効果");
        const core::Layer* blend_if = layer_named(page, "Blend If");
        const core::Layer* plain = layer_named(page, "ふつう");
        QVERIFY(effects != nullptr && blend_if != nullptr && plain != nullptr);
        std::set<std::string> kept;
        for (const auto& [key, ref] : effects->native_psd->blocks) kept.insert(key);
        QCOMPARE(kept, (std::set<std::string>{"lfx2", "lclr", "iOpa", "fxrp", "lnsr"}));  // (not luni, lyid, lspf: this build's own)
        QCOMPARE(effects->native_psd->id, (std::optional<std::int64_t>(7)));
        QVERIFY(effects->native_psd->ranges.empty());
        QVERIFY(!blend_if->native_psd->ranges.empty());
        QVERIFY(core::psd_effects_kept(*effects) && !core::psd_effects_kept(*blend_if));
        // (the locks: 5 = transparency and position, 0x80000000 = all)
        QVERIFY(effects->lock_alpha && effects->lock_position && !effects->locked);
        QVERIFY(blend_if->lock_alpha && blend_if->lock_position && blend_if->locked);
        QVERIFY(!plain->lock_alpha && !plain->lock_position && !plain->locked);
        QVERIFY(std::any_of(opened.notes.begin(), opened.notes.end(), [](const std::string& n) { return n.find("レイヤー効果") != std::string::npos; }));
        QVERIFY(doc.file_info.has_value() && !doc.file_info->author.empty());  // (its XMP's File Info)

        const QString written = path(QStringLiteral("keep/written.psd"));
        QDir().mkpath(path(QStringLiteral("keep")));
        (void)genko::formats::export_page_psd(doc, doc.page(0), genko::storage::path_from_utf8(written.toStdString()), 150);
        const std::string bytes = genko::test::read_bytes(written);
        const psd::File src = psd::File::read(source);
        const psd::File out = psd::File::read(bytes);
        // this build's reader: each layer's kept payloads, its ranges, its id and locks; the page's resources and blocks
        const auto layer_of = [](const psd::File& f, const std::string& name) -> const psd::Layer* {
            for (const psd::Layer& l : f.layers())
                if (l.name == name) return &l;
            return nullptr;
        };
        for (const std::string name : {"効果", "Blend If", "ふつう"}) {
            const psd::Layer* a = layer_of(src, name);
            const psd::Layer* b = layer_of(out, name);
            QVERIFY2(a != nullptr && b != nullptr, name.c_str());
            std::map<std::string, std::string> theirs, ours;
            for (const auto& block : src.blocks(*a)) theirs[block.key] = std::string(block.data);
            for (const auto& block : out.blocks(*b)) ours[block.key] = std::string(block.data);
            for (const auto& [key, data] : theirs) {
                if (key == "luni") continue;
                QVERIFY2(ours.contains(key) && ours[key] == data, (name + " " + key).c_str());
                QVERIFY2(bytes.find(data) != std::string::npos, (name + " " + key).c_str());
            }
            QCOMPARE(std::string(out.ranges(*b)), std::string(src.ranges(*a)));
            QCOMPARE(out.layer_id(*b), src.layer_id(*a));
            QCOMPARE(out.protection(*b), src.protection(*a));
        }
        std::map<unsigned, std::string> src_resources, out_resources;
        for (const auto& r : src.resources()) src_resources[r.id] = std::string(r.data);
        for (const auto& r : out.resources()) out_resources[r.id] = std::string(r.data);
        for (const unsigned id : {1037u, 1049u, 1050u, 1054u, 1065u, 1060u}) QVERIFY2(out_resources[id] == src_resources[id], std::to_string(id).c_str());
        std::map<std::string, std::string> src_globals, out_globals;
        for (const auto& g : src.global_blocks()) src_globals[g.key] = std::string(g.data);
        for (const auto& g : out.global_blocks()) out_globals[g.key] = std::string(g.data);
        for (const std::string key : {"Anno", "Patt"}) QVERIFY2(out_globals[key] == src_globals[key], key.c_str());
        // psd-tools: the same blocks of each layer, the same resources and global blocks
        const Json view = tools_view({QString::fromStdString(keep_path("keep.psd")), written});
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        const Json& before = view[keep_path("keep.psd")];
        const Json& after = view[written.toStdString()];
        const auto tools_layer = [](const Json& v, const std::string& name) {
            for (const Json& l : v["layers"])
                if (l["name"] == name) return l;
            return Json();
        };
        for (const std::string name : {"効果", "Blend If", "ふつう"}) {
            const Json a = tools_layer(before, name), b = tools_layer(after, name);
            QVERIFY2(!a.is_null() && !b.is_null(), name.c_str());
            for (const auto& [key, sha] : a["blocks"].items()) {
                if (key == "luni") continue;
                QVERIFY2(b["blocks"].contains(key) && b["blocks"][key] == sha, (name + " " + key).c_str());
            }
            QCOMPARE(b["ranges"], a["ranges"]);
        }
        for (const std::string id : {"1037", "1049", "1050", "1054", "1065"}) QCOMPARE(after["resources"][id], before["resources"][id]);
        for (const std::string key : {"Anno", "Patt"}) QCOMPARE(after["globals"][key], before["globals"][key]);

        // the book saved and read back: its kept blocks among its assets, the same file written
        const fs::path dir = genko::storage::path_from_utf8(path(QStringLiteral("keep/book.genko")).toStdString());
        {
            genko::storage::ProjectLock lock(dir, "human:test");
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = "human:test";
            request.ops = Json::array();
            genko::storage::Saver(lock).save(doc, request);
        }
        const Json project = Json::parse(genko::test::read_bytes(QString::fromStdString(genko::core::path_to_utf8(dir / "project.json"))));
        QVERIFY(std::find(project["features"].begin(), project["features"].end(), Json("psd-keep@1")) != project["features"].end());
        const genko::storage::LoadResult read = genko::storage::load_document(dir);
        QVERIFY2(read.document.read_only_reason.empty(), read.document.read_only_reason.c_str());
        QVERIFY(read.document.page(0).native_psd == page.native_psd);
        const QString again = path(QStringLiteral("keep/again.psd"));
        (void)genko::formats::export_page_psd(read.document, read.document.page(0), genko::storage::path_from_utf8(again.toStdString()), 150);
        QVERIFY(genko::test::read_bytes(again) == bytes);

        // a layer deleted: the comps and slices that name it are not written; a merge drops what the merged layer kept; a
        // duplicate keeps it, its id written once
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const auto applied = [&](const Json& ops) { return bus.apply(doc, ops, core::Actor("human:test")).doc; };
        const auto resource_ids = [&](const core::Document& d, const QString& name) {
            const QString file = path(QStringLiteral("keep/") + name);
            (void)genko::formats::export_page_psd(d, d.page(0), genko::storage::path_from_utf8(file.toStdString()), 150);
            std::set<unsigned> ids;
            for (const auto& r : psd::File::read(genko::test::read_bytes(file)).resources()) ids.insert(r.id);
            return ids;
        };
        QVERIFY(resource_ids(doc, QStringLiteral("all.psd")).contains(1065));
        const core::Document deleted = applied(Json::array({Json{{"op", "delete_layer"}, {"page", 1}, {"id", plain->id}}}));
        const std::set<unsigned> left = resource_ids(deleted, QStringLiteral("deleted.psd"));
        QVERIFY(!left.contains(1065) && !left.contains(1050) && left.contains(1037) && left.contains(1054));
        const core::Document merged = applied(Json::array({Json{{"op", "merge_down"}, {"page", 1}, {"id", blend_if->id}}}));
        const core::Layer* into = layer_named(merged.page(0), "効果");
        QVERIFY(into != nullptr && !into->native_psd);
        const core::Document twice = applied(Json::array({Json{{"op", "duplicate_layer"}, {"page", 1}, {"id", effects->id}}}));
        int copies = 0;
        for (const core::Layer& l : twice.page(0).layers) copies += l.native_psd && l.native_psd->id == std::optional<std::int64_t>(7) ? 1 : 0;
        QCOMPARE(copies, 2);
        const QString twice_file = path(QStringLiteral("keep/twice.psd"));
        (void)genko::formats::export_page_psd(twice, twice.page(0), genko::storage::path_from_utf8(twice_file.toStdString()), 150);
        int sevens = 0;
        const psd::File twice_read = psd::File::read(genko::test::read_bytes(twice_file));
        for (const psd::Layer& l : twice_read.layers()) sevens += twice_read.layer_id(l) == std::optional<std::uint32_t>(7) ? 1 : 0;
        QCOMPARE(sevens, 1);
        // import_psd's report: the layers keeping effects
        const auto made = bus.apply(core::new_episode("t", core::Num(std::int64_t{1}), 1, core::PageSpec::a4_mono()),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(source)}}}), core::Actor("human:test"));
        QVERIFY2(made.results[0].contains("effects") && made.results[0]["effects"].size() == 1, made.results[0].dump().c_str());
        // the Python reference's view keeps nothing (its import cases above)
        {
            const psd::PythonView python_view;
            const auto viewed = bus.apply(core::new_episode("t", core::Num(std::int64_t{1}), 1, core::PageSpec::a4_mono()),
                                          Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(source)}}}), core::Actor("human:test"));
            QVERIFY(!viewed.doc.page(0).native_psd);
            for (const core::Layer& l : viewed.doc.page(0).layers) QVERIFY(!l.native_psd && !l.lock_position);
        }
    }

    // The layers at their depth (the user's decision 3): psd-tools' files of 16 and 32 bits (RGB, CMYK, Lab, grey; and
    // one keeping its layers in Lr16, as Photoshop writes a 16-bit file) opened as books — a page in precise colour or of
    // inks of that mode and depth — whose layers hold every sample as psd-tools reads it (values no 8-bit sample holds);
    // and import_psd onto such a page of a book.
    void layersAtTheirDepth() {
        namespace core = genko::core;
        struct Case {
            const char* file;
            int colours;
            int depth;
        };
        const std::vector<Case> cases{{"deep-rgb16.psd", 3, 16}, {"deep-rgb32.psd", 3, 32}, {"deep-cmyk16.psd", 4, 16},
                                      {"deep-lab16.psd", 3, 16}, {"deep-gray16.psd", 1, 16}, {"deep-rgb16-lr16.psd", 3, 16}};
        QStringList files;
        for (const Case& c : cases) files << QString::fromStdString(keep_path(c.file));
        const Json view = tools_view(files, true);
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        for (const Case& c : cases) {
            const std::string name = c.file;
            const genko::formats::Opened opened = genko::formats::open_file(keep_file(name), name);
            const core::Page& page = opened.doc.page(0);
            QCOMPARE(page.layers.size(), std::size_t{2});
            const Json& tools = view[keep_path(name)];
            for (const core::Layer& layer : page.layers) {
                Json theirs;
                for (const Json& l : tools["layers"])
                    if (l["name"] == layer.title) theirs = l;
                QVERIFY2(!theirs.is_null(), (name + " " + layer.title).c_str());
                const int left = theirs["box"][0].get<int>(), top = theirs["box"][1].get<int>(), right = theirs["box"][2].get<int>();
                const int width = right - left;
                const auto expected = [&](int channel, int x, int y) {
                    const Json& values = theirs["values"][std::to_string(channel)];
                    const double v = values[static_cast<std::size_t>((y - top) * width + (x - left))].get<double>();
                    const double normal = c.depth == 16 ? v / 65535.0 : v;
                    return c.colours == 4 && channel >= 0 ? 1.0 - normal : normal;  // (CMYK stored as no ink 1)
                };
                const auto at = [&](int x, int y, int channel, double value) {
                    const double want = expected(channel, x, y);
                    const double tolerance = c.depth == 16 ? 0.25 / 65535.0 : 1e-6;
                    QVERIFY2(std::fabs(value - want) <= tolerance,
                             (name + " " + layer.title + " " + std::to_string(x) + "," + std::to_string(y) + " ch " + std::to_string(channel) + ": " +
                              std::to_string(value) + " for " + std::to_string(want))
                                 .c_str());
                };
                const int bottom = theirs["box"][3].get<int>();
                if (c.colours == 3 && name.find("lab") == std::string::npos) {
                    QVERIFY2(page.rgb && page.rgb->depth == c.depth && !page.color && layer.color_raster, name.c_str());
                    const core::ColorRasterView raster(*layer.color_raster);
                    for (int y = top; y < bottom; ++y) {
                        for (int x = left; x < right; ++x) {
                            const auto px = raster.pixel(static_cast<std::size_t>(y) * raster.width() + static_cast<std::size_t>(x));
                            for (int ch = 0; ch < 3; ++ch) at(x, y, ch, px[static_cast<std::size_t>(ch)]);
                            at(x, y, -1, px[3]);
                        }
                    }
                } else {
                    QVERIFY2(page.color && page.color->depth == c.depth && layer.ink_raster, name.c_str());
                    const core::InkPixels inks = core::decode_ink_raster(*layer.ink_raster);
                    for (int y = top; y < bottom; ++y) {
                        for (int x = left; x < right; ++x) {
                            const std::size_t i = static_cast<std::size_t>(y) * inks.width + static_cast<std::size_t>(x);
                            for (int ch = 0; ch < c.colours; ++ch) at(x, y, ch, inks.value(i, static_cast<unsigned>(ch)));
                            at(x, y, -1, inks.value(i, static_cast<unsigned>(c.colours)));
                        }
                    }
                }
            }
        }
        // import_psd onto a page in precise colour of 16 bits of a book (its paper the file's pixels)
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const std::string rgb16 = keep_file("deep-rgb16.psd");
        const double w = 8 * 25.4 / 300, h = 6 * 25.4 / 300;
        const core::Document book = core::new_episode("t", core::Num(std::int64_t{1}), 1, core::PageSpec::custom(w, h, w, h, 0, 0, 0, 0, 0, 300, "color"));
        const auto done = bus.apply(book,
                                    Json::array({Json{{"op", "set_color_mode"}, {"page", 1}, {"mode", "rgb"}, {"depth", 16}},
                                                 Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(rgb16)}, {"fit", "paper"}, {"id", "deep"}}}),
                                    core::Actor("human:test"));
        int deep = 0;
        for (const core::Layer& l : done.doc.page(0).layers) deep += l.id.starts_with("deep-") && l.color_raster ? 1 : 0;
        QCOMPARE(deep, 2);
    }

    // PSB (Photoshop's large document): a page written as a PSB — its layers, kept blocks and resources, an 8-bit, a
    // 16-bit and a CMYK page, a smart object's file in the global lnk2 of an 8-byte length — that psd-tools reads (version
    // 2, the same layers and blocks) and this build's reader reads (its layers' pictures those of the page's PSD).
    void psbIsWritten() {
        namespace core = genko::core;
        const QString dir = path(QStringLiteral("psb"));
        QDir().mkpath(dir);
        QStringList files;
        for (const std::string name : {"keep.psd", "deep-rgb16.psd", "deep-cmyk16.psd"}) {
            const genko::formats::Opened opened = genko::formats::open_file(keep_file(name), name);
            const QString psd_file = dir + QLatin1Char('/') + QString::fromStdString(name);
            const QString psb_file = dir + QLatin1Char('/') + QString::fromStdString(name.substr(0, name.size() - 4)) + QStringLiteral(".psb");
            (void)genko::formats::export_page_psd(opened.doc, opened.doc.page(0), genko::storage::path_from_utf8(psd_file.toStdString()));
            const genko::formats::CopyResult copied = genko::formats::write_copy(opened.doc, 0, genko::storage::path_from_utf8(psb_file.toStdString()));
            QVERIFY(copied.file.extension() == ".psb");
            const std::string psd_bytes = genko::test::read_bytes(psd_file), psb_bytes = genko::test::read_bytes(psb_file);
            QCOMPARE(psb_bytes.substr(0, 6), std::string("8BPS\0\2", 6));
            const psd::File a = psd::File::read(psd_bytes, psd::default_limits(), true), b = psd::File::read(psb_bytes, psd::default_limits(), true);
            QVERIFY(b.psb() && !a.psb());
            QCOMPARE(b.layers().size(), a.layers().size());
            for (std::size_t i = 0; i < a.layers().size(); ++i) {
                QCOMPARE(b.layers()[i].name, a.layers()[i].name);
                if (a.layers()[i].folder || a.layers()[i].kind != "pixels") continue;
                QVERIFY2(b.image(b.layers()[i]).tobytes() == a.image(a.layers()[i]).tobytes(), (name + " " + a.layers()[i].name).c_str());
                std::vector<std::string> ka, kb;
                for (const auto& block : a.blocks(a.layers()[i])) ka.push_back(block.key + std::string(block.data));
                for (const auto& block : b.blocks(b.layers()[i])) kb.push_back(block.key + std::string(block.data));
                QVERIFY(ka == kb);
            }
            QVERIFY(b.merged_picture()->tobytes() == a.merged_picture()->tobytes());
            files << psd_file << psb_file;
        }
        // a smart object's file: lnk2 with its 8-byte length
        genko::formats::Opened image = genko::formats::open_file(keep_file("keep.psd"), "keep.psd");
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const std::string png = genko::test::read_bytes(genko::test::repo_root() + QStringLiteral("/native/tests/data/pyref/png/rgb8.png"));
        const auto placed = bus.apply(image.doc, Json::array({Json{{"op", "smart_place"}, {"page", 1}, {"data", core::b64encode(png)}, {"name", "絵.png"}}}),
                                      core::Actor("human:test"));
        const QString smart_psb = dir + QStringLiteral("/smart.psb");
        (void)genko::formats::export_page_psd(placed.doc, placed.doc.page(0), genko::storage::path_from_utf8(smart_psb.toStdString()), std::nullopt, {}, nullptr,
                                              true);
        const std::string smart_bytes = genko::test::read_bytes(smart_psb);
        QVERIFY(smart_bytes.find(std::string("8BIMlnk2\0\0\0\0", 12)) != std::string::npos);  // (its length's high 4 bytes)
        const psd::File smart = psd::File::read(smart_bytes);
        bool found = false;
        for (const psd::Layer& l : smart.layers()) found = found || (l.placed && smart.linked(l.placed->uuid).has_value());
        QVERIFY(found);
        files << smart_psb;
        const Json view = tools_view(files);
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        for (const QString& file : files) {
            const Json& v = view[file.toStdString()];
            QCOMPARE(v["version"].get<int>(), file.endsWith(QStringLiteral(".psb")) ? 2 : 1);
        }
        for (int k = 0; k + 1 < 6; k += 2) {
            const Json& a = view[files[k].toStdString()];
            const Json& b = view[files[k + 1].toStdString()];
            QCOMPARE(b["layers"].size(), a["layers"].size());
            for (std::size_t i = 0; i < a["layers"].size(); ++i) {
                QCOMPARE(b["layers"][i]["name"], a["layers"][i]["name"]);
                QCOMPARE(b["layers"][i]["blocks"], a["layers"][i]["blocks"]);
                QCOMPARE(b["layers"][i]["channels"], a["layers"][i]["channels"]);
            }
            QCOMPARE(b["resources"], a["resources"]);
            QCOMPARE(b["globals"], a["globals"]);
        }
    }

    // A page past what a PSD holds (30000 px a side) is refused as a PSD — before anything is written, saying to write a PSB —
    // and written as a PSB, which both readers read back at its size.
    void aPageTooLargeForAPsdIsAPsb() {
        namespace core = genko::core;
        genko::formats::NewImage wide;
        wide.title = "横長";
        wide.width = 30001;
        wide.height = 2;
        wide.dpi = 72;
        const genko::formats::Opened opened = genko::formats::new_image(wide);
        const QString psd_file = path(QStringLiteral("wide.psd")), psb_file = path(QStringLiteral("wide.psb"));
        try {
            (void)genko::formats::export_page_psd(opened.doc, opened.doc.page(0), genko::storage::path_from_utf8(psd_file.toStdString()), 72);
            QFAIL("a PSD of 30001 px written");
        } catch (const core::Error& error) {
            QCOMPARE(error.code(), std::string("psd_too_large"));
            QVERIFY2(std::string(error.what()).find("PSB") != std::string::npos, error.what());
        }
        QVERIFY(!QFile::exists(psd_file));
        (void)genko::formats::export_page_psd(opened.doc, opened.doc.page(0), genko::storage::path_from_utf8(psb_file.toStdString()), 72, {}, nullptr, true);
        const psd::File read = psd::File::read(genko::test::read_bytes(psb_file));
        QVERIFY(read.psb() && read.width() == 30001 && read.height() == 2);
        const Json view = tools_view({psb_file});
        QVERIFY2(!view.contains("error"), view.dump().c_str());
        QCOMPARE(view[psb_file.toStdString()]["width"].get<int>(), 30001);
        QCOMPARE(view[psb_file.toStdString()]["version"].get<int>(), 2);
        // the command line: --format psb writes PSBs (a folder: one a page), --format psd refuses the page
        const QString book = path(QStringLiteral("wide.genko"));
        {
            const std::filesystem::path dir = genko::storage::path_from_utf8(book.toStdString());
            genko::storage::ProjectLock lock(dir, "human:test");
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = "human:test";
            request.ops = Json::array();
            genko::storage::Saver(lock).save(opened.doc, request);
        }
        const auto psb_run = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("wide-out")), QStringLiteral("--format"),
                                                     QStringLiteral("psb"), QStringLiteral("--dpi"), QStringLiteral("72"), QStringLiteral("--json")});
        QVERIFY2(psb_run.exit_code == 0, psb_run.err.constData());
        const Json listed = Json::parse(psb_run.out.toStdString());
        QVERIFY2(listed["files"].size() == 1 && listed["files"][0].get<std::string>().ends_with("_p001.psb"), listed.dump().c_str());
        const auto psd_run = genko::test::run_genko({QStringLiteral("export"), book, path(QStringLiteral("wide-psd")), QStringLiteral("--format"),
                                                     QStringLiteral("psd"), QStringLiteral("--dpi"), QStringLiteral("72")});
        QVERIFY2(psd_run.exit_code != 0 && (psd_run.out + psd_run.err).contains("PSB"), (psd_run.out + psd_run.err).constData());
    }

    // A PSD without what this build does not read (a file of psd-tools' layers alone, this build's own PSD) keeps nothing:
    // no native_psd, no psd-keep@1 — its PSD is written as before.
    void aPsdOfNothingUnknownKeepsNothing() {
        namespace core = genko::core;
        const genko::formats::Opened opened = genko::formats::open_file(keep_file("deep-rgb16.psd"), "deep-rgb16.psd");
        QVERIFY(!opened.doc.page(0).native_psd);
        for (const core::Layer& l : opened.doc.page(0).layers) QVERIFY(!l.native_psd);
        QVERIFY(!core::uses_psd_keep(opened.doc));
        // this build's PSD of a book read back, written again: the same bytes
        const QString first = path(QStringLiteral("own1.psd")), second = path(QStringLiteral("own2.psd"));
        (void)genko::formats::export_page_psd(opened.doc, opened.doc.page(0), genko::storage::path_from_utf8(first.toStdString()));
        const genko::formats::Opened again = genko::formats::open_file(genko::test::read_bytes(first), "own1.psd");
        QVERIFY(!again.doc.page(0).native_psd);
        for (const core::Layer& l : again.doc.page(0).layers) QVERIFY(!l.native_psd);
        (void)genko::formats::export_page_psd(again.doc, again.doc.page(0), genko::storage::path_from_utf8(second.toStdString()));
        QVERIFY(genko::test::read_bytes(first).size() > 0);
    }

    // What a hostile PSD says of its blocks is not believed: a layer of more blocks than one keeps keeps 4096 (and says
    // so), blocks and Lr16 whose lengths run past the file are read as far as they go or not at all, a canvas past what an
    // op makes is refused before its deep picture is made, a PSB of a header past the limits is not opened.
    void hostileBlocksAreBounded() {
        namespace core = genko::core;
        // a layer of 5000 small unknown blocks
        std::string many;
        for (int i = 0; i < 5000; ++i) many += std::string("8BIMxyz") + static_cast<char>('a' + i % 26) + std::string("\0\0\0\0", 4);
        const std::string file = psd_with_blocks(many, 4, 4);
        const genko::core::CommandBus bus(genko::render::ops_registry());
        const auto made = bus.apply(core::new_episode("t", core::Num(std::int64_t{1}), 1, core::PageSpec::a4_mono()),
                                    Json::array({Json{{"op", "import_psd"}, {"page", 1}, {"psd", core::b64encode(file)}}}), core::Actor("human:test"));
        QVERIFY2(made.results[0].value("psd_blocks_cut", false), made.results[0].dump().c_str());
        for (const core::Layer& l : made.doc.page(0).layers)
            if (l.native_psd) QVERIFY(l.native_psd->blocks.size() <= core::kPsdKeptMax);
        // a block whose length runs past the file: the reader stops there (no block past the file is kept)
        std::string past = std::string("8BIMabcd") + std::string("\x7f\xff\xff\xff", 4) + "abcd";
        const std::string cut = psd_with_blocks(past, 4, 4);
        try {
            (void)psd::File::read(cut);
        } catch (const psd::ReadError&) {
        }
        // an Lr16 of a length past the file, and one of a layer count past what it holds: read as a flat file
        const std::string lr16 = flat_with_global(std::string("8BIMLr16") + std::string("\0\0\x10\0", 4) + std::string("\x7f\xff", 2), 4, 4);
        const psd::File flat = psd::File::read(lr16);
        QVERIFY(flat.layers().empty());
        // a canvas past what an op makes: refused before the picture at its depth is made
        const std::string huge = psd_with_blocks({}, 60000, 60000, 16);
        try {
            const psd::File read = psd::File::read(huge);
            for (const psd::Layer& l : read.layers()) (void)read.layer_deep(l);
            QFAIL("a deep picture of 60000 × 60000 made");
        } catch (const psd::ReadError& error) {
            QVERIFY(error.too_large());
        }
        // a PSB whose header says 300000 × 300000: not opened
        std::string psb = huge;
        psb[5] = 2;
        try {
            (void)genko::formats::open_file(psb, "huge.psb");
            QFAIL("opened");
        } catch (const core::Error& error) {
            QVERIFY2(error.code() == "image_too_large" || error.code() == "format", error.code().c_str());
        }
    }
};

QTEST_GUILESS_MAIN(TestContractPsd)
#include "test_contract_psd.moc"
