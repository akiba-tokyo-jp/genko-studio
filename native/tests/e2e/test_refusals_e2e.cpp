// What this build refuses on purpose (SPEC COMP-01a), on a book it makes itself and through its command line.
//
// refusedAsThisBuildRefuses: the cases of e2e/refusal_cases.json — a number that is not finite (text such as "inf" or
// "nan" taken for a number) in a paper, its margins, a line, its tail, style, cuts and outline, a placement, a layout's
// rect or a reported face; a frame_id that is not a panel's id; a picture that cannot be drawn (a line's style picture)
// or read (a material library's picture that is no picture, outside the library, named by a whole path or through a
// link; a placed picture cut short, broken or too large to open); a book import_pages cannot read (a newer version,
// JSON that is not a book, a picture missing); a paint layer moved onto a basic frame of no finite size or onto a paper
// too large for its pixels; what only a person holds changed by an agent. Each case's steps applied one after another,
// the steps before its last done, its last refused with the case's code, its error holding the case's words: through the
// command bus on the book in memory, and through `genko apply` on a copy of the book on disk, with nothing written (the
// book's files as they were).
//
// peopleOnlyThroughTheCommandLine: what only a person holds refused to an agent through `genko apply` with nothing
// written (the book's files and studio/audit.jsonl as they were), a person's done; an approved character a person
// deletes recorded in studio/audit.jsonl as an approval gone, its Undo refused to an agent (it brings an approval back)
// and done by a person, recorded so.
//
// brokenPicturesAreNotPlaced: a picture whose pixels cannot be decoded (a JPEG, a BMP and a GIF cut short in their
// pixels: their size in the header) not placed through `genko apply` — by place_asset, adopt_candidate, nor set_placement
// where a book written elsewhere placed it —, each batch refused, an agent's and a person's, nothing written; and the page
// such a book placed them on neither drawn (`genko render`) nor exported (`genko export`): the error says so, and
// nothing is written.
//
// whatThisBuildDoesOnPurpose: the user's decisions D1 (set_page_spec puts each page's pixels on its own paper, a cover's
// too, and moves a layer's mask with what it masks) and D2 (a page taken in by import_pages, or copied, names its own
// new layers where it named the other page's).

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "broken_pictures.hpp"
#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/model.hpp"
#include "core/pynum.hpp"
#include "render/image.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "testsupport.hpp"

using genko::core::Actor;
using genko::core::Document;
using genko::core::Json;
using genko::core::Num;
namespace fs = std::filesystem;
namespace render = genko::render;
namespace pictures = genko::test::pictures;

namespace {

const genko::core::CommandBus& bus() {
    static const genko::core::CommandBus b(render::ops_registry());
    return b;
}

const char* const kPerson = "human:作者";
const char* const kAgent = "ai:hermes";

fs::path fpath(const QString& text) { return genko::storage::path_from_utf8(text.toStdString()); }

// The steps of a case: "steps", or one step of "ops" and "agent".
Json steps_of(const Json& c) {
    if (c.contains("steps")) return c["steps"];
    Json step = Json::object();
    step["ops"] = c["ops"];
    if (c.contains("agent")) step["agent"] = c["agent"];
    return Json::array({step});
}

// Every text of `value` with `names` put in (the longest names first: $PSDX before $PSD).
Json placed(const Json& value, const std::map<std::string, std::string, std::greater<>>& names) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (const auto& [name, put] : names) {
            for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + put.size())) text.replace(at, name.size(), put);
        }
        return text;
    }
    Json out = value;
    if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) out[i] = placed(value[i], names);
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) out[key] = placed(item, names);
    }
    return out;
}

std::uint32_t crc32(const std::string& bytes) {
    std::uint32_t crc = 0xffffffffu;
    for (const char ch : bytes) {
        crc ^= static_cast<unsigned char>(ch);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::string be32(std::uint32_t v) {
    return std::string{static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
}

// A PNG chunk of `type` holding `data`.
std::string chunk(const std::string& type, const std::string& data) { return be32(static_cast<std::uint32_t>(data.size())) + type + data + be32(crc32(type + data)); }

// The person's material library (<config>/materials) stamp_material reads: pictures (u-pic, and ones that are not
// there, hold nothing, are a folder, are not a picture, lie in a folder of the library or outside it, are named by a
// whole path or are links), letterings in folders of their own (with a library's odd values: numbers as text, a style as
// pairs, keys and sizes not taken), and materials of a kind there is not.
void write_library(const QString& config) {
    const QString root = config + QStringLiteral("/materials");
    QDir().mkpath(root + QStringLiteral("/sub"));
    render::Image picture = render::Image::create("RGBA", {6, 3}, render::Ink{20, 120, 220, 255});
    picture.paste(render::Ink{250, 40, 10, 128}, render::Box{0, 0, 3, 3});
    render::save_png(picture, fpath(root + QStringLiteral("/u-pic.png")));
    render::save_png(picture, fpath(config + QStringLiteral("/outside.png")));
    render::save_png(picture, fpath(root + QStringLiteral("/sub/u-pic.png")));
    QFile::link(config + QStringLiteral("/outside.png"), root + QStringLiteral("/link.png"));
    QFile::link(config + QStringLiteral("/nowhere.png"), root + QStringLiteral("/dangling.png"));
    genko::test::write_bytes(root + QStringLiteral("/empty.png"), "");
    genko::test::write_bytes(root + QStringLiteral("/notes.png"), "not a picture");
    const std::string outside = (config + QStringLiteral("/outside.png")).toStdString();
    const std::string nowhere = (config + QStringLiteral("/nowhere.png")).toStdString();
    const Json png = "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFElEQVR4nGM8ISf3n4GBgYGJAQoAHxICB2m00JwAAAAASUVORK5CYII=";
    const Json items = Json::array({
        Json{{"id", "u-pic"}, {"kind", "image"}, {"name", "青い絵"}, {"folder", "画像"}, {"file", "u-pic.png"}, {"width_mm", 30},
             {"aspect", 0.5}},
        Json{{"id", "u-pic-missing"}, {"kind", "image"}, {"name", "ない絵"}, {"file", "nothing.png"}},
        Json{{"id", "u-pic-nofile"}, {"kind", "image"}, {"name", "ファイルのない絵"}},
        Json{{"id", "u-pic-empty"}, {"kind", "image"}, {"name", "空の絵"}, {"file", "empty.png"}},
        Json{{"id", "u-pic-number"}, {"kind", "image"}, {"name", "番号の絵"}, {"file", 5}},
        Json{{"id", "u-pic-folder"}, {"kind", "image"}, {"name", "フォルダの絵"}, {"file", "sub"}},
        Json{{"id", "u-pic-text"}, {"kind", "image"}, {"name", "絵でない"}, {"file", "notes.png"}},
        Json{{"id", "u-pic-outside"}, {"kind", "image"}, {"name", "外の絵"}, {"file", "../outside.png"}},
        Json{{"id", "u-pic-whole-path"}, {"kind", "image"}, {"name", "全体のパスの絵"}, {"file", outside}},
        Json{{"id", "u-pic-sub"}, {"kind", "image"}, {"name", "フォルダの中の絵"}, {"file", "sub/u-pic.png"}},
        Json{{"id", "u-pic-sub-missing"}, {"kind", "image"}, {"name", "フォルダの中にない絵"}, {"file", "sub/nothing.png"}},
        Json{{"id", "u-pic-outside-missing"}, {"kind", "image"}, {"name", "外にない絵"}, {"file", "../nothing.png"}},
        Json{{"id", "u-pic-whole-path-missing"}, {"kind", "image"}, {"name", "全体のパスにない絵"}, {"file", nowhere}},
        Json{{"id", "u-pic-link"}, {"kind", "image"}, {"name", "リンクの絵"}, {"file", "link.png"}},
        Json{{"id", "u-pic-dangling"}, {"kind", "image"}, {"name", "先のないリンクの絵"}, {"file", "dangling.png"}},
        Json{{"id", "u-let-plain"}, {"kind", "lettering"}, {"name", "ただの描き文字"}, {"folder", "自作の描き文字"}},
        Json{{"id", "u-let-vertical"}, {"kind", "lettering"}, {"name", "ズキューン"}, {"folder", "効果音/強い"}, {"text", "ズキューン"},
             {"balloon", "shout"}, {"wrap", "vertical"},
             {"style", Json{{"size_mm", "7.5"}, {"bold", 1}, {"rgb", Json::array({200, 0, 0})}}},
             {"w_mm", "30"}, {"h_mm", 80}},
        Json{{"id", "u-let-picture"}, {"kind", "lettering"}, {"name", "絵の描き文字"}, {"text", "絵"},
             {"style", Json{{"picture", png}, {"font", nullptr}, {"tracking", ""}}}},
        Json{{"id", "u-let-infinite"}, {"kind", "lettering"}, {"name", "無限の幅"}, {"w_mm", "inf"}},
        Json{{"id", "u-bogus"}, {"kind", "bogus"}, {"name", "知らない種類"}},
        Json{{"id", "u-no-kind"}, {"name", "種類のない素材"}},
    });
    genko::test::write_bytes(root + QStringLiteral("/library.json"), genko::core::dump_python(items));
}

// `doc` saved by storage::Saver as a new v4 book in `dir` (its assets already there or written by the save).
void save_new(const Document& doc, const fs::path& dir, const std::string& actor) {
    fs::create_directories(dir);
    genko::storage::ProjectLock lock(dir, actor);
    lock.try_acquire();
    genko::storage::SaveRequest request;
    request.actor = actor;
    request.ops = Json::array();
    genko::storage::Saver(lock).save(doc, request);
}

Document applied(const Document& doc, const Json& ops, const std::string& actor = kPerson) {
    return bus().apply(doc, ops, Actor(actor)).doc;
}

const genko::core::Page* page_numbered(const Document& doc, std::int64_t index) {
    for (const auto& page : doc.pages) {
        if (page->index == Num(index)) return page.get();
    }
    return nullptr;
}

const genko::core::Layer* layer_named(const genko::core::Page& page, const std::string& id) {
    for (const genko::core::Layer& layer : page.layers) {
        if (layer.id == id) return &layer;
    }
    return nullptr;
}

// D2 (the user's decision): every reference to a layer on each page — a layer's folder, a ruler's layer, an area kept on
// the page ({"layer": id}, also inside a union, intersect or subtract), a line's style.below_layer — names a layer of
// that page. "" when it holds (and something was looked at); else where it does not.
std::string layer_refs_difference(const Document& doc) {
    int checked = 0;
    for (const auto& page : doc.pages) {
        std::set<std::string> ids;
        for (const genko::core::Layer& layer : page->layers) ids.insert(layer.id);
        std::string wrong;
        const auto check = [&](const Json& value, const std::string& what) {
            if (!value.is_string() || value.get_ref<const std::string&>().empty()) return;
            ++checked;
            if (!ids.contains(value.get<std::string>()) && wrong.empty()) wrong = what + " " + value.get<std::string>();
        };
        for (const genko::core::Layer& layer : page->layers) check(layer.parent_id, "layer " + layer.id + "'s folder");
        if (page->rulers.is_array()) {
            for (const Json& ruler : page->rulers) {
                if (ruler.is_object() && ruler.contains("layer_id")) check(ruler["layer_id"], "a ruler's layer");
            }
        }
        const std::function<void(const Json&)> area_refs = [&](const Json& area) {
            if (!area.is_object()) return;
            if (area.contains("layer")) check(area["layer"], "a kept area's layer");
            for (const char* key : {"union", "intersect", "subtract"}) {
                if (area.contains(key) && area[key].is_array()) {
                    for (const Json& part : area[key]) area_refs(part);
                }
            }
        };
        if (page->extra.is_object() && page->extra.contains("saved_areas") && page->extra["saved_areas"].is_object()) {
            for (const auto& [name, area] : page->extra["saved_areas"].items()) area_refs(area);
        }
        for (const genko::core::StoryLine* line : doc.story_for_page(page->index)) {
            if (line->style.is_object() && line->style.contains("below_layer")) check(line->style["below_layer"], "line " + line->id + "'s layer");
        }
        if (!wrong.empty()) return "page " + page->index.repr() + ": " + wrong + " is not a layer of the page";
    }
    if (checked == 0) return "no reference to a layer to check";
    return {};
}

// fill_area with the area kept on page `index` under `name` ({"saved": name}) done: "" when it is.
std::string kept_area_fills(const Document& doc, std::int64_t index, const std::string& name) {
    const Json ops = Json::array({Json::object({{"op", "fill_area"}, {"page", index}, {"area", Json::object({{"saved", name}})}, {"rgb", Json::array({0, 0, 0})}})});
    try {
        (void)bus().apply(doc, ops, Actor(std::string(genko::core::kLegacyActor)));
    } catch (const genko::core::ApplyError& error) {
        return "fill_area of the area kept as " + name + " on page " + std::to_string(index) + ": " + error.what();
    }
    return {};
}

struct Spread {
    double area_mm2 = 0;
    double cx = 0;
    double cy = 0;
};

// The part of a picture that `inked` picks: its area and middle in mm (kx, ky px per mm).
template <class Inked>
Spread spread_of(const render::Image& image, double kx, double ky, Inked inked) {
    const std::string bytes = image.tobytes();
    const int bands = image.mode() == std::string("L") ? 1 : 4;
    double n = 0, sx = 0, sy = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const auto* px = reinterpret_cast<const unsigned char*>(bytes.data()) +
                             (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width()) + static_cast<std::size_t>(x)) * bands;
            if (!inked(px)) continue;
            n += 1;
            sx += x + 0.5;
            sy += y + 0.5;
        }
    }
    if (n == 0) return {};
    return Spread{n / (kx * ky), sx / n / kx, sy / n / ky};
}

// D1 (the user's decision): the layer's pixels (alpha over 127) or its mask's hidden part (under 128) moved with the
// page's basic frame from `before` to `after`, as everything else on the page: their middle where the frame takes it
// (within 0.5 mm) and their area scaled with it (within 5%), the picture as big as the page's own paper (200 dpi; a mask
// 150 dpi) — none of it cut off. "" when it holds.
std::string moved_difference(const Document& before, const Document& after, std::int64_t index, const std::string& id, const std::string& what) {
    const genko::core::Page* a = page_numbered(before, index);
    const genko::core::Page* b = page_numbered(after, index);
    if (a == nullptr || b == nullptr) return "no page " + std::to_string(index);
    const genko::core::Layer* la = layer_named(*a, id);
    const genko::core::Layer* lb = layer_named(*b, id);
    if (la == nullptr || lb == nullptr) return "no layer " + id + " on page " + std::to_string(index);
    const bool paint = what == "paint";
    const auto picture = [&](const genko::core::Layer& layer) -> std::optional<render::Image> {
        const genko::core::Bytes& png = paint ? layer.raster_png : (layer.mask ? layer.mask->png : genko::core::Bytes());
        if (!png || png->empty()) return std::nullopt;
        return render::open_image(*png, render::kPillowOpenLimits).convert(paint ? "RGBA" : "L");
    };
    const auto old_picture = picture(*la);
    const auto new_picture = picture(*lb);
    if (!old_picture || !new_picture) return what + " of layer " + id + " missing";
    const int dpi = paint ? 200 : 150;
    const int width = paint ? render::mm_to_px(b->spec.width_mm.value(), dpi)
                            : static_cast<int>(std::max(1.0, genko::core::py_round_whole(b->spec.width_mm.value() / 25.4 * dpi)));
    const int height = paint ? render::mm_to_px(b->spec.height_mm.value(), dpi)
                             : static_cast<int>(std::max(1.0, genko::core::py_round_whole(b->spec.height_mm.value() / 25.4 * dpi)));
    if (new_picture->width() != width || new_picture->height() != height) {
        return what + " of layer " + id + " is " + std::to_string(new_picture->width()) + "x" + std::to_string(new_picture->height()) +
               ", not as big as its page's paper (" + std::to_string(width) + "x" + std::to_string(height) + ")";
    }
    const auto inked = [paint](const unsigned char* px) { return paint ? px[3] > 127 : px[0] < 128; };
    const double k = paint ? 200 / 25.4 : 0;
    const Spread was = spread_of(*old_picture, paint ? k : old_picture->width() / a->spec.width_mm.value(),
                                 paint ? k : old_picture->height() / a->spec.height_mm.value(), inked);
    const Spread now = spread_of(*new_picture, paint ? k : new_picture->width() / b->spec.width_mm.value(),
                                 paint ? k : new_picture->height() / b->spec.height_mm.value(), inked);
    if (was.area_mm2 <= 0) return what + " of layer " + id + " had nothing to move";
    const genko::core::Rect from = a->inner_rect_mm(before.start_side.value_or(""));
    const genko::core::Rect to = b->inner_rect_mm(after.start_side.value_or(""));
    const double sx = to.width.value() / from.width.value();
    const double sy = to.height.value() / from.height.value();
    const double want_x = to.x.value() + (was.cx - from.x.value()) * sx;
    const double want_y = to.y.value() + (was.cy - from.y.value()) * sy;
    const double want_area = was.area_mm2 * sx * sy;
    char text[400];
    std::snprintf(text, sizeof text, "%s of layer %s: %.2f mm2 about (%.2f, %.2f) mm, where %.2f mm2 about (%.2f, %.2f) mm moved with the frame",
                  what.c_str(), id.c_str(), now.area_mm2, now.cx, now.cy, want_area, want_x, want_y);
    if (std::fabs(now.area_mm2 - want_area) > 0.05 * want_area || std::hypot(now.cx - want_x, now.cy - want_y) > 0.5) return text;
    return {};
}

}  // namespace

class TestRefusalsE2e : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;
    QString book_;    // the book the cases are applied to (v4, saved by this build)
    QString config_;  // the config folder with the material library
    std::map<std::string, std::string, std::greater<>> names_;  // a case's names ($A1, a panel's id of the cases) → this book's

    QString path(const QString& relative) const { return tmp_.path() + QLatin1Char('/') + relative; }

    // The pictures, put into the book's assets: $A1 … $A3, $S1, $S2 and $F1 whole PNGs; $JX, $BX, $GX, $CUT a JPEG, a BMP,
    // a GIF and a PNG cut short in their pixels; $PSDX a Photoshop file cut short in its merged picture; $BAD bytes that
    // are no picture; $CRC a PNG whose header's checksum is wrong; $BOMB a PNG whose header is too large to open.
    void put_pictures(const fs::path& dir) {
        genko::storage::AssetStore store(dir);
        const auto put = [&](const char* name, const std::string& bytes) { names_["$" + std::string(name)] = store.put_bytes(bytes, ".png"); };
        int n = 0;
        for (const char* name : {"A1", "A2", "A3", "S1", "S2", "F1"}) {
            render::Image image = render::Image::create("RGB", {64, 48}, render::Ink{30 * n, 200 - 20 * n, 90});
            image.paste(render::Ink{250, 250, 250}, render::Box{4 + n, 4, 20 + n, 30});
            put(name, render::write_png(image));
            ++n;
        }
        put("JX", genko::core::a2b_base64(pictures::kSamples[0].cut));
        put("BX", genko::core::a2b_base64(pictures::kSamples[1].cut));
        put("GX", genko::core::a2b_base64(pictures::kSamples[2].cut));
        put("CUT", genko::core::a2b_base64(pictures::kSamples[3].cut));
        put("PSDX", genko::core::a2b_base64(pictures::kPsdCut));
        put("BAD", "these bytes are no picture");
        std::string crc = genko::core::a2b_base64(pictures::kSamples[3].whole);
        crc[29] = static_cast<char>(crc[29] ^ 0x5a);  // (IHDR's checksum)
        put("CRC", crc);
        const std::string header = be32(30000) + be32(30000) + std::string{8, 2, 0, 0, 0};
        put("BOMB", std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", std::string("\x78\x9c\x03\x00\x00\x00\x00\x01", 8)) +
                        chunk("IEND", ""));
    }

    // The book: six pages; page 1 of two panels, its name approved, its first panel with a person's region, three
    // candidates and the art adopted from cd_a1, a line in it; page 4 a paint layer with pixels (paint-1); page 6 a line;
    // the studio: c-yuki, c-ken's sheet and face approved by a person (locked), the style fixed from page 6, a request for
    // approval of page 1's name (ticket 000000000a0c) and a fix for the agent.
    void make_book() {
        const fs::path dir = fpath(book_);
        fs::create_directories(dir);
        put_pictures(dir);
        Document doc = genko::core::new_episode("断る原稿", Num(1), 6, genko::core::PageSpec::a4_mono());
        doc.asset_dir = dir;
        doc = applied(doc, Json::parse(R"([{"op": "split_frame", "page": 1, "axis": "horizontal", "ratio": 0.5, "gutter_mm": 5}])"));
        const auto leaves = doc.page(0).leaf_frames();
        QCOMPARE(leaves.size(), std::size_t{2});
        names_["000000000026"] = leaves[0]->id;
        names_["000000000029"] = leaves[1]->id;
        names_["000000000018"] = doc.page(3).leaf_frames().at(0)->id;
        names_["000000000045"] = doc.page(5).leaf_frames().at(0)->id;
        doc.edit_page(0).name_ok = true;
        const Json made = placed(Json::parse(R"([
            {"op": "import_candidates", "page": 1, "frame_id": "000000000026", "candidates": [
                {"id": "cd_a1", "asset": "$A1", "px": [64, 48], "origin": {"kind": "agent"}},
                {"id": "cd_a2", "asset": "$A2", "px": [64, 48], "origin": {"kind": "agent"}},
                {"id": "cd_a3", "asset": "$A3", "px": [64, 48], "origin": {"kind": "agent"}}]},
            {"op": "adopt_candidate", "page": 1, "frame_id": "000000000026", "candidate_id": "cd_a1"},
            {"op": "set_panel", "page": 1, "frame_id": "000000000026", "set": {"regions": [{"id": "rg_a", "kind": "face", "rect_mm": [10, 10, 30, 30], "source": "user"}]}},
            {"op": "add_line", "page": 1, "text": "台詞", "frame_id": "000000000026", "x_mm": 40, "y_mm": 40},
            {"op": "add_line", "page": 6, "text": "台詞", "x_mm": 50, "y_mm": 60},
            {"op": "add_layer", "page": 4, "kind": "pen", "id": "paint-1"},
            {"op": "add_stroke", "page": 4, "layer_id": "paint-1", "points": [[40, 60], [90, 120], [140, 100]], "width_mm": 6},
            {"op": "convert_layer", "page": 4, "id": "paint-1", "to": "paint"}
        ])"),
                                 names_);
        doc = applied(doc, made);
        const auto line_on = [&doc](int page) { return doc.story_for_page(Num(page)).at(0)->id; };
        names_["00000000002b"] = line_on(1);
        names_["000000000047"] = line_on(6);
        doc.bible.characters = placed(Json::parse(R"([
            {"id": "c-yuki", "name": "ユキ"},
            {"id": "c-ken", "name": "健", "locked": true,
             "refs": [{"asset": "$S1", "kind": "sheet", "approved_by": "human:作者"}, {"asset": "$F1", "kind": "face", "approved_by": "human:作者"}]}])"),
                                      names_);
        doc.studio = Json::parse(R"({"style": {"notes": ["細い線"], "locked": {"from_page": "p6", "rev": 3}}})");
        doc.tickets = Json::array({Json::parse(R"({"id": "000000000a0c", "kind": "gate", "item": "name", "pages": [1], "status": "open", "assignee": "human"})"),
                                   Json::parse(R"({"id": "000000000a0d", "kind": "fix", "status": "open", "assignee": "agent"})")});
        save_new(doc, dir, kPerson);
        const std::string problems = genko::test::book_problems(book_);
        QVERIFY2(problems.empty(), problems.c_str());
    }

    // <scratch>/…: books import_pages cannot read — a newer version, JSON that is not a book, a book missing a picture
    // (its paint layer's).
    void write_unreadable_books() {
        const QString dir = path("scratch");
        QDir().mkpath(dir + "/newer.genko");
        QDir().mkpath(dir + "/notbook.genko");
        genko::test::write_bytes(dir + "/newer.genko/project.json", R"({"version": 99, "title": "x", "pages": []})");
        genko::test::write_bytes(dir + "/notbook.genko/project.json", "[]");
        Document other = genko::core::new_episode("他の本", Num(1), 2, genko::core::PageSpec::a4_mono());
        other = applied(other, Json::parse(R"([
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "o-paint"},
            {"op": "add_stroke", "page": 2, "layer_id": "o-paint", "points": [[30, 30], [80, 90]], "width_mm": 5},
            {"op": "convert_layer", "page": 2, "id": "o-paint", "to": "paint"}])"));
        const QString damaged = dir + "/damaged.genko";
        save_new(other, fpath(damaged), kPerson);
        const Json payload = genko::test::read_json(damaged + "/project.json");
        int removed = 0;
        for (const Json& layer : payload["pages"][1]["layers"]) {
            if (layer["id"] != Json("o-paint")) continue;
            const std::string rel = genko::storage::AssetStore::relpath(layer["asset"].get<std::string>(), ".png");
            QVERIFY(QFile::remove(damaged + "/" + QString::fromStdString(rel)));
            ++removed;
        }
        QCOMPARE(removed, 1);
        names_["$SCRATCH"] = dir.toStdString();
        names_["$PAGESBOOK"] = book_.toStdString();
    }

    // `genko apply` of `ops` on `book` as `agent`.
    genko::test::Run apply_cli(const QString& book, const Json& ops, const std::string& agent) {
        static int n = 0;
        const QString file = path(QStringLiteral("ops-%1.json").arg(++n));
        genko::test::write_bytes(file, genko::core::dump_python(ops));
        QProcessEnvironment env;
        env.insert(QStringLiteral("GENKO_CONFIG_DIR"), config_);
        return genko::test::run_genko({"apply", book, file, "--agent", QString::fromStdString(agent)}, env);
    }

    std::vector<Json> audit(const QString& book) const {
        std::vector<Json> out;
        QFile file(book + "/studio/audit.jsonl");
        if (!file.open(QIODevice::ReadOnly)) return out;
        for (const QByteArray& line : file.readAll().split('\n')) {
            if (!line.trimmed().isEmpty()) out.push_back(genko::core::parse_python_json(line.toStdString()));
        }
        return out;
    }

private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
        config_ = path("config");
        write_library(config_);
        qputenv("GENKO_CONFIG_DIR", config_.toUtf8());
        book_ = path("book.genko");
        make_book();
        write_unreadable_books();
    }

    void refusedAsThisBuildRefuses() {
        const Json file = genko::test::read_json(genko::test::repo_root() + "/native/tests/e2e/refusal_cases.json");
        const Json& cases = file["cases"];
        QCOMPARE(cases.size(), std::size_t{64});
        const auto loaded = genko::storage::load_document(fpath(book_));
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        std::map<std::string, int> refused;  // group → cases refused as they say
        std::vector<std::string> failures;
        for (std::size_t n = 0; n < cases.size(); ++n) {
            const Json& c = cases[n];
            const std::string name = c["n"].get<std::string>();
            const Json steps = placed(steps_of(c), names_);
            const std::string code = c["cpp"].get<std::string>();
            const std::string says = c["cpp_says"].get<std::string>();
            // through the command bus, on the book in memory
            Document doc = loaded.document;
            std::string wrong;
            for (std::size_t s = 0; s < steps.size() && wrong.empty(); ++s) {
                const std::string agent = steps[s].value("agent", std::string(genko::core::kLegacyActor));
                const bool last = s + 1 == steps.size();
                try {
                    Document after = bus().apply(doc, steps[s]["ops"], Actor(agent)).doc;
                    if (last) wrong = "the bus applied it";
                    doc = std::move(after);
                } catch (const genko::core::ApplyError& error) {
                    if (!last) {
                        wrong = "step " + std::to_string(s) + " refused: " + error.what();
                    } else if (error.code() != code || std::string(error.what()).find(says) == std::string::npos) {
                        wrong = "the bus refused it with " + error.code() + ": " + error.what();
                    }
                }
            }
            // through the command line, on a copy of the book on disk: nothing written
            if (wrong.empty()) {
                const QString copy = path(QStringLiteral("cases/%1.genko").arg(n));
                genko::test::copy_tree(book_, copy);
                for (std::size_t s = 0; s < steps.size() && wrong.empty(); ++s) {
                    const std::string agent = steps[s].value("agent", std::string(genko::core::kLegacyActor));
                    const bool last = s + 1 == steps.size();
                    const auto before = genko::test::tree_hashes(copy, false);
                    const auto run = apply_cli(copy, steps[s]["ops"], agent);
                    const Json reply = genko::test::one_line(run.out);
                    if (!last) {
                        if (run.exit_code != 0) wrong = "genko apply refused step " + std::to_string(s) + ": " + run.described().toStdString();
                        continue;
                    }
                    if (run.exit_code != 1 || !reply.is_object() || reply.value("code", std::string()) != code ||
                        reply.value("error", std::string()).find(says) == std::string::npos) {
                        wrong = "genko apply gave " + run.described().toStdString();
                    } else if (genko::test::tree_hashes(copy, false) != before) {
                        wrong = "genko apply refused it and wrote to the book";
                    }
                }
            }
            if (!wrong.empty()) {
                failures.push_back(name + ": should be refused with " + code + " (" + says + "); " + wrong.substr(0, 1200));
                continue;
            }
            ++refused[c["group"].get<std::string>()];
        }
        for (const auto& f : failures) qWarning("%s", f.c_str());
        QVERIFY2(failures.empty(), (std::to_string(failures.size()) + " of " + std::to_string(cases.size()) + " cases not refused as they say").c_str());
        QCOMPARE(refused["book"], 8);
        QCOMPARE(refused["lines"], 20);
        QCOMPARE(refused["studio"], 36);
    }

    // What this build does otherwise on purpose, the user's decisions: D2 — import_pages and duplicate_page point what
    // names a layer on a page taken in or copied (a layer's folder, a ruler kept to a layer, the areas kept on the page,
    // alone and inside a union, a subtract and an intersect) at its own new layers, the areas kept still filled from —
    // also after the book is saved and read back; D1 — set_page_spec puts each page's pixels on its own new paper (a
    // jacket's and a band's, wider than the book's, kept whole) and moves a layer's mask with what it masks.
    void whatThisBuildDoesOnPurpose() {
        // the other book: page 2 with a folder and a layer in it, a ruler kept to that layer, a paint layer, and areas
        // kept on the page that name its layers
        Document other = genko::core::new_episode("他の本", Num(1), 2, genko::core::PageSpec::a4_mono());
        other = applied(other, Json::parse(R"([
            {"op": "add_layer", "page": 2, "kind": "folder", "id": "o-folder"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "o-child", "parent": "o-folder"},
            {"op": "add_stroke", "page": 2, "layer_id": "o-child", "points": [[20, 20], [60, 40]]},
            {"op": "add_ruler", "page": 2, "kind": "line", "points": [[0, 0], [50, 50]], "layer_id": "o-child"},
            {"op": "add_layer", "page": 2, "kind": "pen", "id": "o-paint"},
            {"op": "add_stroke", "page": 2, "layer_id": "o-paint", "points": [[30, 30], [80, 90]], "width_mm": 5},
            {"op": "convert_layer", "page": 2, "id": "o-paint", "to": "paint"}])"));
        {
            std::string ink;
            for (const genko::core::Layer& layer : other.page(1).layers) {
                if (layer.role == genko::core::LayerRole::Ink) ink = layer.id;
            }
            QVERIFY(!ink.empty());
            const Json box = Json::object({{"rect", Json::array({10, 10, 30, 20})}});
            const Json corner = Json::object({{"rect", Json::array({0, 0, 40, 40})}});
            const Json paint = Json::object({{"layer", "o-paint"}});
            const Json lines = Json::object({{"layer", ink}, {"grow_mm", 1}});
            const Json inside = Json::object({{"intersect", Json::array({paint, corner})}});
            other.edit_page(1).extra["saved_areas"] = Json::object({{"paint", paint},
                                                                    {"lines", Json::object({{"union", Json::array({box, lines})}})},
                                                                    {"nested", Json::object({{"subtract", Json::array({Json::object({{"all", true}}), inside})}})}});
        }
        const QString other_dir = path("other.genko");
        save_new(other, fpath(other_dir), kPerson);
        const auto loaded = genko::storage::load_document(fpath(book_));
        QVERIFY2(loaded.report.clean(), genko::core::dump_python(loaded.report.to_json()).c_str());
        const std::int64_t pages = static_cast<std::int64_t>(loaded.document.pages.size());
        // D2: taken in, and its copy
        Document doc = applied(loaded.document, Json::array({Json::object({{"op", "import_pages"}, {"from", other_dir.toStdString()}, {"pages", Json::array({2})}})}),
                             genko::core::kLegacyActor.data());
        const std::int64_t taken = pages + 1;
        QVERIFY(page_numbered(doc, taken) != nullptr);
        QCOMPARE(layer_refs_difference(doc), std::string());
        for (const char* name : {"paint", "lines", "nested"}) QCOMPARE(kept_area_fills(doc, taken, name), std::string());
        doc = applied(doc, Json::array({Json::object({{"op", "duplicate_page"}, {"page", taken}})}), genko::core::kLegacyActor.data());
        QCOMPARE(static_cast<std::int64_t>(doc.pages.size()), pages + 2);
        QCOMPARE(layer_refs_difference(doc), std::string());
        for (const auto& page : doc.pages) {
            if (page->index.value() <= static_cast<double>(pages)) continue;
            for (const char* name : {"paint", "lines", "nested"}) QCOMPARE(kept_area_fills(doc, static_cast<std::int64_t>(page->index.value()), name), std::string());
        }
        // saved and read back
        const QString saved = path("taken-in.genko");
        genko::test::copy_tree(book_, saved);
        {
            genko::storage::ProjectLock lock(fpath(saved), kPerson);
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = kPerson;
            request.base_revision = loaded.document.revision;
            request.ops = Json::array();
            request.assets_from = fpath(other_dir);
            genko::storage::Saver(lock).save(doc, request);
        }
        const auto back = genko::storage::load_document(fpath(saved));
        QVERIFY2(back.report.clean(), genko::core::dump_python(back.report.to_json()).c_str());
        QCOMPARE(layer_refs_difference(back.document), std::string());
        for (const char* name : {"paint", "lines", "nested"}) QCOMPARE(kept_area_fills(back.document, taken, name), std::string());

        // D1: a jacket's paint on its own paper; a layer's mask with its layer; a band's paint and its mask on a custom paper
        struct Moving {
            const char* cover;   // add_cover's settings, or none
            const char* layer;   // the layer whose pixels move
            const char* mask;    // set_layer_mask's area on it, or none
            const char* spec;    // set_page_spec's settings
            std::vector<std::string> moved;
        };
        const Moving moving[] = {
            {R"({"kind": "jacket", "spine_mm": 12, "flap_mm": 60})", "jacket-art", nullptr, R"({"preset": "a5"})", {"paint"}},
            {nullptr, "paint-1", R"({"rect": [40, 60, 50, 40]})", R"({"preset": "b4"})", {"mask"}},
            {R"({"kind": "obi", "spine_mm": 9, "flap_mm": 30, "height_mm": 60})", "band-art", R"({"rect": [400, 30, 30, 20]})",
             R"({"paper": [150, 210], "trim": [140, 200], "bleed_mm": 5, "margins": [10, 10, 10, 10]})", {"paint", "mask"}},
        };
        for (const Moving& m : moving) {
            Document before = loaded.document;
            std::int64_t index = 4;
            if (m.cover != nullptr) {
                Json cover = Json::parse(m.cover);
                cover["op"] = "add_cover";
                before = applied(before, Json::array({cover}), genko::core::kLegacyActor.data());
                index = static_cast<std::int64_t>(before.pages.back()->index.value());
                const std::vector<Json> drawn{Json::parse(R"([[440, 100], [470, 140], [455, 180]])"), Json::parse(R"([[410, 20], [425, 35], [430, 45]])")};
                before = applied(before, Json::array({Json::object({{"op", "add_layer"}, {"page", index}, {"kind", "pen"}, {"id", m.layer}}),
                                                    Json::object({{"op", "add_stroke"}, {"page", index}, {"layer_id", m.layer},
                                                                  {"points", drawn[std::string(m.layer) == "jacket-art" ? 0 : 1]}, {"width_mm", 6}}),
                                                    Json::object({{"op", "convert_layer"}, {"page", index}, {"id", m.layer}, {"to", "paint"}})}),
                               genko::core::kLegacyActor.data());
            }
            if (m.mask != nullptr) {
                before = applied(before, Json::array({Json::object({{"op", "set_layer_mask"}, {"page", index}, {"id", m.layer}, {"area", Json::parse(m.mask)}, {"invert", true}})}),
                               genko::core::kLegacyActor.data());
            }
            Json spec = Json::parse(m.spec);
            spec["op"] = "set_page_spec";
            const Document after = applied(before, Json::array({spec}), genko::core::kLegacyActor.data());
            for (const std::string& what : m.moved) {
                const std::string wrong = moved_difference(before, after, index, m.layer, what);
                QVERIFY2(wrong.empty(), (std::string(m.layer) + ": " + wrong).c_str());
            }
        }
    }

    void peopleOnlyThroughTheCommandLine() {
        const QString v4 = path("people.genko");
        genko::test::copy_tree(book_, v4);
        const Json tries = placed(Json::parse(R"([
            [{"op": "set_panel", "page": 1, "frame_id": "000000000026", "set": {"regions": []}}],
            [{"op": "set_panel", "page": 1, "frame_id": "000000000026", "unset": ["regions"]}],
            [{"op": "set_bible", "characters": [{"id": "c-yuki", "name": "ユキ"}]}],
            [{"op": "delete_character", "id": "c-ken", "force": true}],
            [{"op": "set_ticket", "id": "000000000a0c", "status": "done"}],
            [{"op": "set_studio", "style": {"locked": null}}],
            [{"op": "upsert_character", "character": {"id": "c-yuki", "name": "ユキ", "locked": true}}],
            [{"op": "upsert_character", "character": {"id": "c-new", "name": "新", "locked": true}}]
        ])"),
                                  names_);
        for (const Json& ops : tries) {
            const auto before = genko::test::tree_hashes(v4, false);
            const auto run = apply_cli(v4, ops, kAgent);
            const Json reply = genko::test::one_line(run.out);
            const std::string what = genko::core::dump_python(ops) + ": " + run.out.toStdString();
            QVERIFY2(run.exit_code == 1 && reply.value("code", std::string()) == "apply", what.c_str());
            QVERIFY2(reply["error"].get<std::string>().find("only a person can") != std::string::npos, what.c_str());
            QVERIFY2(genko::test::tree_hashes(v4, false) == before, what.c_str());  // (nothing written, the audit as it was)
        }
        // a person deletes c-ken, whose sheet a person approved: the approval gone is in the audit
        const std::size_t lines = audit(v4).size();
        const auto deleted = apply_cli(v4, tries[3], kPerson);
        QVERIFY2(deleted.exit_code == 0, deleted.out.constData());
        auto after = audit(v4);
        QCOMPARE(after.size(), lines + 1);
        QCOMPARE(after.back()["actor"], Json(kPerson));
        QCOMPARE(after.back()["changes"], Json::parse(R"([{"what": "character:c-ken:locked", "from": true, "to": null}])"));
        // its Undo brings the approval back: not an agent's to do; a person's, recorded so
        const auto before_undo = genko::test::tree_hashes(v4, false);
        const auto by_agent = genko::test::run_genko({"undo", v4, "--as", kAgent, "--force"});  // (--force: past "another's change")
        QVERIFY2(by_agent.exit_code != 0 && genko::test::one_line(by_agent.out).value("code", std::string()) == "needs_person",
                 by_agent.out.constData());
        QVERIFY(genko::test::tree_hashes(v4, false) == before_undo);
        const auto by_person = genko::test::run_genko({"undo", v4, "--as", kPerson});
        QVERIFY2(by_person.exit_code == 0, by_person.out.constData());
        after = audit(v4);
        QCOMPARE(after.back()["changes"], Json::parse(R"([{"what": "character:c-ken:locked", "from": null, "to": true}])"));
        // and the rest a person does
        for (const std::size_t k : {0, 1, 2, 4, 5, 6, 7}) {
            const auto run = apply_cli(v4, tries[k], kPerson);
            QVERIFY2(run.exit_code == 0, (genko::core::dump_python(tries[k]) + ": " + run.out.toStdString()).c_str());
        }
        const std::string problems = genko::test::book_problems(v4);
        QVERIFY2(problems.empty(), problems.c_str());
    }

    void brokenPicturesAreNotPlaced() {
        // a book written elsewhere places the three on page 1's second panel (nothing adopted there): as art, background
        // and draft — this build places none of them, so they are written into the book as such a book holds them
        const QString v4 = path("broken.genko");
        genko::test::copy_tree(book_, v4);
        {
            Document doc = genko::storage::load_document(fpath(v4)).document;
            const std::string frame = names_.at("000000000029");
            int n = 0;
            for (const auto& [name, to] : std::vector<std::pair<std::string, std::string>>{{"$JX", "art"}, {"$BX", "bg"}, {"$GX", "draft"}}) {
                genko::core::Layer layer;
                layer.id = "ly_placed_" + std::to_string(n++);
                layer.role = to == "draft" ? genko::core::LayerRole::Draft : (to == "bg" ? genko::core::LayerRole::Bg : genko::core::LayerRole::User);
                layer.kind = genko::core::LayerKind::Placed;
                layer.exportable = to != "draft";
                layer.title = to + " " + frame;
                layer.frame_id = frame;
                layer.asset = names_.at(name);
                layer.source = Json::object({{"asset", names_.at(name)}, {"to", to}, {"by", kAgent}});
                layer.placement_mm = genko::core::Rect{Num(20), Num(160), Num(90), Num(60)};
                layer.fit = "cover";
                layer.clip_to = "frame";
                auto& page = doc.edit_page(0);
                page.layers.insert(page.layers.begin(), std::move(layer));
            }
            genko::storage::ProjectLock lock(fpath(v4), kAgent);
            lock.try_acquire();
            genko::storage::SaveRequest request;
            request.actor = kAgent;
            request.base_revision = doc.revision;
            request.ops = Json::array();
            genko::storage::Saver(lock).save(doc, request);
        }
        const auto refused = [&](const Json& ops, const char* agent) {
            const auto before = genko::test::tree_hashes(v4, false);
            const auto run = apply_cli(v4, ops, agent);
            const Json reply = genko::test::one_line(run.out);
            const std::string what = std::string(agent) + " " + genko::core::dump_python(ops) + ": " + run.out.toStdString();
            QVERIFY2(run.exit_code == 1 && reply.value("code", std::string()) == "apply", what.c_str());
            QVERIFY2(reply["error"].get<std::string>().find("the asset's picture cannot be read (") != std::string::npos, what.c_str());
            QVERIFY2(genko::test::tree_hashes(v4, false) == before, what.c_str());  // (nothing written, the audit as it was)
        };
        for (const char* name : {"$JX", "$BX", "$GX"}) {
            const std::string ref = names_.at(name);
            const std::string panel = names_.at("000000000026");
            const Json place = Json::array({Json::object({{"op", "place_asset"}, {"page", 4}, {"asset", ref}})});
            const Json adopt = Json::array({Json::object({{"op", "import_candidates"}, {"page", 1}, {"frame_id", panel},
                                                          {"candidates", Json::array({Json::object({{"id", "cd_x"}, {"asset", ref},
                                                                                                    {"px", Json::array({9, 6})},
                                                                                                    {"origin", Json::object({{"kind", "agent"}})}})})}}),
                                            Json::object({{"op", "adopt_candidate"}, {"page", 1}, {"frame_id", panel}, {"candidate_id", "cd_x"}})});
            for (const char* agent : {kAgent, kPerson}) {
                refused(place, agent);
                refused(adopt, agent);
            }
        }
        for (const char* to : {"art", "bg", "draft"}) {
            const Json fit = Json::array({Json::object({{"op", "set_placement"}, {"page", 1}, {"frame_id", names_.at("000000000029")}, {"to", to}, {"scale", 0.5}})});
            refused(fit, kAgent);
            refused(fit, kPerson);
        }
        // the page they are placed on: neither drawn nor exported, nothing written
        const QString png = path("page1.png");
        const auto drawn = genko::test::run_genko({"render", v4, "--page", "1", "--dpi", "40", "--out", png});
        const Json drawn_reply = genko::test::one_line(drawn.out);
        QVERIFY2(drawn.exit_code != 0 && drawn_reply.value("code", std::string()) == "format", (drawn.out + drawn.err).constData());
        QVERIFY(!QFileInfo::exists(png));
        const QString out = path("exported");
        const auto exported = genko::test::run_genko({"export", v4, out, "--format", "png", "--dpi", "40"});
        const Json exported_reply = genko::test::one_line(exported.out);
        QVERIFY2(exported.exit_code != 0 && !exported_reply.value("ok", true) && exported_reply.value("code", std::string()) == "format",
                 (exported.out + exported.err).constData());
        QVERIFY2(!QFileInfo::exists(out) || QDir(out).isEmpty(), out.toUtf8().constData());
    }
};

QTEST_GUILESS_MAIN(TestRefusalsE2e)
#include "test_refusals_e2e.moc"
