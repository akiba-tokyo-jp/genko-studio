// StudioService's art tools (Python's genko/studio/service.py: render … upscale, import_image): previews, generation
// requests and their guides, imports, candidates, adoption with enlargement, reported regions, the finishing pass and
// Genko's own image steps.

#include <algorithm>
#include <limits>
#include <cmath>

#include "core/base64.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "formats/pillow_save.hpp"
#include "render/draw.hpp"
#include "render/filters.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/raster_ops.hpp"
#include "render/text/fonts.hpp"
#include "service/atari.hpp"
#include "service/book.hpp"
#include "service/briefs.hpp"
#include "service/finish.hpp"
#include "service/genreq.hpp"
#include "service/guide.hpp"
#include "service/importer.hpp"
#include "service/jobs.hpp"
#include "service/preflight.hpp"
#include "service/preview.hpp"
#include "service/service_util.hpp"
#include "service/sheets.hpp"
#include "service/studio_service.hpp"
#include "service/upscale.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"
#include "studio/files.hpp"
#include "studio/state.hpp"

namespace genko::service {

using core::Num;
using core::py_get;
using core::py_str;
using core::py_truthy;

namespace {

constexpr std::int64_t kMaxImagePixels = 120'000'000;  // ops.MAX_IMAGE_PIXELS
constexpr std::uintmax_t kMaxImportBytes = 64ULL * 1024 * 1024;

const char* const kStandingNotes[] = {"composition と pose は構図の参考。線をなぞらせる必要はない", "画像に文字・フキダシ・効果音を描かせない。台詞は Genko が描く"};

int clamp_dpi(double value, int low, int high) {
    if (std::isnan(value)) throw core::PyValueError("cannot convert float NaN to integer");
    if (std::isinf(value)) throw core::PyUncaught("OverflowError", "cannot convert float infinity to integer");
    return static_cast<int>(std::max<double>(low, std::min<double>(high, std::trunc(value))));
}

// max_px / (length_mm / 25.4)
double per_inch(const Json& max_px, const Num& length_mm) {
    const double inches = (length_mm / Num(25.4)).value();
    if (inches == 0.0) throw core::PyUncaught("ZeroDivisionError", "float division by zero");
    return core::to_num(max_px).value() / inches;
}

const core::Frame& frame_or_apply_error(const core::Page& page, const Json& page_index, const Json& frame_id) {
    try {
        return briefs::find_frame(page, Json(py_str(frame_id)));
    } catch (const core::PyUncaught& e) {
        if (e.type() != "KeyError" && e.type() != "IndexError") throw;
        std::string ids;
        for (const core::Frame* f : page.leaf_frames()) ids += (ids.empty() ? "" : ", ") + f->id;
        throw core::ApplyError(py_str(page_index) + " ページにコマ " + py_str(frame_id) + " はない（このページのコマ: " + ids + "）");
    }
}

}  // namespace

// _render_kind(episode, project, page_index, frame_id, kind, candidate_id, max_px): the PNG and its label.
std::pair<std::string, std::string> render_kind(const core::Document& doc, const fs::path& project, const Json& page_index, const Json& frame_id,
                                                const Json& kind, const Json& candidate_id, const Json& max_px) {
    const double side = core::to_num(max_px).value();
    const auto exact_px = [side](double aspect) {
        // previews keep the exact aspect (the request sizes are rounded to 64 for image tools)
        const int width = std::max(16, static_cast<int>(core::py_round_int(std::pow(side * side / 2 * aspect, 0.5))));
        return std::array<int, 2>{width, std::max(16, static_cast<int>(core::py_round_int(width / aspect)))};
    };
    const auto at = page_at(doc, page_index);
    if (!at) throw core::ApplyError(py_str(page_index) + " ページはない");
    const core::Page& page = doc.page(*at);
    const core::Frame& frame = frame_or_apply_error(page, page_index, frame_id);
    const Json panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
    render::Image image;
    std::string label;
    if (core::py_equals(kind, Json("compare"))) {
        const Json cand_id = py_truthy(candidate_id) ? candidate_id : py_get(util::or_dict(py_get(panel, "adopted")), "art");
        Json cand;
        for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) {
            if (core::py_equals(util::item(c, "id"), cand_id)) {
                cand = c;
                break;
            }
        }
        if (cand.is_null()) throw core::ApplyError("候補 " + py_str(cand_id) + " はない");
        const auto data = storage::AssetStore(project).get_bytes(py_str(util::item(cand, "asset")), ".png");
        if (!data) throw core::ApplyError("候補 " + py_str(cand_id) + " の画像が assets/ に無い");
        const double pad = core::to_float(py_get(util::or_dict(py_get(cand, "mapping")), "pad_mm", Json(0.0)));
        const double aspect = (frame.rect.width.value() + 2 * pad) / (frame.rect.height.value() + 2 * pad);
        const guide::GenBox box = guide::GenBox::for_frame(frame, pad, exact_px(aspect));
        image = guide::compare(open_picture(*data), page, frame, box);
        label = "compare_" + frame.id + "_" + py_str(cand_id);
    } else if (core::is_one_of(kind, {"guide:composition", "guide:pose", "guide:keepout"})) {
        const double aspect = (frame.rect.width.value() + 2 * genreq::kPadMm) / (frame.rect.height.value() + 2 * genreq::kPadMm);
        const guide::GenBox box = guide::GenBox::for_frame(frame, genreq::kPadMm, exact_px(aspect));
        const std::string name = kind.get<std::string>().substr(6);
        image = name == "keepout" ? guide::keepout(doc, page, frame, box) : name == "pose" ? guide::pose(page, frame, box) : guide::composition(page, frame, box);
        label = name + "_" + frame.id;
    } else {
        throw core::ApplyError("kind は compare / guide:composition / guide:pose / guide:keepout");
    }
    // (thumbnail only shrinks: an edge past an int's range is the int's end — the same picture, not a size cut to its
    // low bits)
    const std::int64_t asked = core::to_int(max_px);
    const int edge = static_cast<int>(std::clamp<std::int64_t>(asked, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    image.thumbnail(render::Size{edge, edge});
    return {png(image), label};
}

namespace {

// _guide_sheet(pack): the request's guides side by side, long side 512 px.
std::optional<std::string> guide_sheet(const genreq::Pack& pack) {
    std::vector<render::Image> images;
    for (const char* name : {"guides/composition.png", "guides/pose.png", "guides/keepout.png"}) {
        if (pack.files.count(name) != 0) images.push_back(open_picture(pack.files.at(name)).convert("L"));
    }
    if (images.empty()) return std::nullopt;
    const int h = std::min(256, images.front().height());
    std::vector<render::Image> thumbs;
    int total = 0;
    for (const render::Image& img : images) {
        thumbs.push_back(img.resize(render::Size{std::max(1, static_cast<int>(core::py_round_int(static_cast<double>(img.width()) * h / img.height()))), h}));
        total += thumbs.back().width();
    }
    render::Image sheet = render::Image::create("L", render::Size{total + 8 * (static_cast<int>(thumbs.size()) - 1), h}, render::Ink(200));
    int x = 0;
    for (const render::Image& thumb : thumbs) {
        sheet.paste(thumb, render::Point{x, 0});
        x += thumb.width() + 8;
    }
    sheet.thumbnail(render::Size{512, 512});
    return png(sheet);
}

// _contact_sheet(project, items): up to 8 candidates as tiles with their ids.
std::optional<std::string> contact_sheet(const fs::path& project, const std::vector<Json>& items) {
    const storage::AssetStore store(project);
    std::vector<render::Image> tiles;
    render::TrueTypeFonts fonts;
    for (std::size_t i = 0; i < items.size() && i < 8; ++i) {
        const Json& cand = items[i];
        const auto data = store.get_bytes(py_str(util::item(cand, "asset")), ".png");
        if (!data) continue;
        render::Image img = open_picture(*data).convert("RGB");
        img.thumbnail(render::Size{192, 192});
        render::Image tile = render::Image::create("RGB", render::Size{192, 208}, render::Ink{255, 255, 255});
        tile.paste(img, render::Point{(192 - img.width()) / 2, 16 + (192 - img.height()) / 2});
        render::Draw draw(tile);
        draw.text(render::PointD{2, 2}, render::text::u32(py_str(util::item(cand, "id"))), fonts.load_default(), render::Ink{0, 0, 0});
        tiles.push_back(std::move(tile));
    }
    if (tiles.empty()) return std::nullopt;
    const int cols = std::min<int>(4, static_cast<int>(tiles.size()));
    const int rows = (static_cast<int>(tiles.size()) + cols - 1) / cols;
    render::Image sheet = render::Image::create("RGB", render::Size{cols * 196, rows * 212}, render::Ink{220, 220, 220});
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        sheet.paste(tiles[i], render::Point{static_cast<int>(i % static_cast<std::size_t>(cols)) * 196, static_cast<int>(i / static_cast<std::size_t>(cols)) * 212});
    }
    return png(sheet);
}

Json record_from_request(const Json& request) {
    const Json target = util::or_dict(py_get(request, "target"));
    Json kept = Json::object();
    for (const char* key : {"page", "page_id", "frame_id", "character_id", "location_id"}) {
        if (target.contains(key)) kept[key] = target[key];
    }
    return Json::object({{"id", util::item(request, "id")},
                         {"purpose", py_get(request, "purpose")},
                         {"mode", py_get(request, "mode")},
                         {"target", std::move(kept)},
                         {"brief_hash", py_get(request, "brief_hash")},
                         {"parent", py_get(request, "parent")},
                         {"tool", py_get(request, "tool")},
                         {"pad_mm", py_get(util::or_dict(py_get(request, "size")), "pad_mm", Json(0.0))}});
}

std::vector<Issue> joined(std::vector<Issue> a, const std::vector<Issue>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// base64.b64decode(text, validate=True) (Python 3.12's binascii.a2b_base64 in strict mode): nothing for text that is
// not base64.
std::optional<std::string> b64decode_validate(std::string_view text) {
    if (!text.empty() && text.front() == '=') return std::nullopt;  // (leading padding)
    int quad = 0;
    int pads = 0;
    bool padding = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '=') {
            padding = true;
            if (quad >= 2 && quad + ++pads >= 4) {
                if (i + 1 < text.size()) return std::nullopt;  // (data after the padding)
                return core::a2b_base64(text);
            }
            continue;
        }
        const bool digit = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' || ch == '/';
        if (!digit || padding) return std::nullopt;  // (only base64 data; no data in the middle of the padding)
        pads = 0;
        quad = (quad + 1) % 4;
    }
    if (quad != 0) return std::nullopt;  // (incorrect padding)
    return core::a2b_base64(text);
}

// _png(image) of a picture an agent brought in: a palette picture (a GIF, mode P) stored as Pillow stores it, a palette
// PNG; a mode Pillow does not write as PNG (PA) is Python's OSError.
std::string picture_png(const render::Image& image) {
    try {
        return png(image);
    } catch (const core::Error& error) {
        if (error.code() == "io") throw core::PyUncaught("OSError", error.what());
        throw;
    }
}

// Path(text).stem
std::string stem_of(const std::string& text) {
    const fs::path p = core::path_from_utf8(text);
    std::string name = core::path_to_utf8(p.filename());
    if (name.empty() || name == "." || name == "..") return name;
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) return name;
    return name.substr(0, dot);
}

}  // namespace

ToolResult StudioService::render(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const Json& page = a["page"];
    const Json& mode = a["mode"];
    if (!core::is_one_of(mode, {"name", "proof", "print"})) return fail("mode " + py_str(mode) + " はない（name / proof / print）", "bad_mode", "/mode");
    const Json& max_px = a["max_px"];
    const Json& frame_id = a["frame_id"];
    const Json& kind = a["kind"];
    if (py_truthy(a["layer_id"])) {
        const auto at = page_at(doc, page);
        if (!at) return fail(py_str(page) + " ページはない", "no_page", "/page");
        const core::Page& target = doc.page(*at);
        const core::Layer* layer = nullptr;
        for (const core::Layer& l : target.layers) {
            if (core::py_equals(Json(l.id), a["layer_id"])) {
                layer = &l;
                break;
            }
        }
        if (layer == nullptr) return fail(py_str(page) + " ページにレイヤー " + py_str(a["layer_id"]) + " はない", "no_layer", "/layer_id");
        const int dpi = clamp_dpi(per_inch(max_px, target.spec.height_mm), 36, 300);
        const render::Image alone = render::layer_image(target, *layer, dpi, &doc);
        render::Image paper = render::Image::create("RGBA", alone.size(), render::Ink{255, 255, 255, 255});
        paper.alpha_composite(alone);
        const std::string bytes = png(paper.convert("RGB"));
        // (C++: the id's "/" and "\\" written "_", and the file kept in the book — SPEC COMP-01a)
        const fs::path out =
            path / "studio" / "reviews" / core::path_from_utf8("p" + util::zero3(page) + "_layer_" + util::name_part(py_str(a["layer_id"])) + ".png");
        if (!util::inside(root_, out)) return fail("書き出し先が --root の外になる: " + util::path_text(out), "path_outside_root", "/layer_id");
        util::write_file(out, bytes);
        const std::string title = layer->title.empty() ? std::string(core::to_string(layer->role)) : layer->title;
        return ToolResult(true, Json::object({{"page", page}, {"layer_id", a["layer_id"]}, {"title", title}}), {}, {bytes}, {util::path_text(out)});
    }
    std::string bytes;
    std::string label;
    if (core::py_equals(kind, Json("atari"))) {
        const auto at = page_at(doc, page);
        if (!at) return fail(py_str(page) + " ページはない", "no_page", "/page");
        const core::Page& target = doc.page(*at);
        const int dpi = clamp_dpi(per_inch(max_px, target.spec.height_mm), 36, 200);
        bytes = png(atari::overlay_image(doc, target, dpi));
        label = "atari";
    } else if (py_truthy(kind)) {
        std::tie(bytes, label) = render_kind(doc, path, page, frame_id, kind, a["candidate_id"], max_px);
    } else {
        const auto draft = util::name_of(doc, page);
        bytes = preview(doc, page, mode.get<std::string>(), max_px, draft ? py_get(*draft, "name") : Json(), frame_id);
        label = py_truthy(frame_id) ? mode.get<std::string>() + "_" + py_str(frame_id) : mode.get<std::string>();
    }
    std::replace(label.begin(), label.end(), ':', '-');
    // (C++: the ids in the label — the candidate's, the frame's — with their "/" and "\\" written "_", and the file kept
    // in the book: Python writes wherever a "../" in an id leads — SPEC COMP-01a)
    const fs::path out = path / "studio" / "reviews" / core::path_from_utf8("p" + util::zero3(page) + "_" + util::name_part(label) + ".png");
    if (!util::inside(root_, out)) return fail("書き出し先が --root の外になる: " + util::path_text(out), "path_outside_root", "/");
    util::write_file(out, bytes);
    return ToolResult(true, Json::object({{"page", page}, {"mode", mode}, {"kind", kind}, {"frame_id", frame_id}}), {}, {bytes}, {util::path_text(out)});
}

Json StudioService::told_once(const fs::path& path, const Json& request) const {
    const fs::path marker = path / "studio" / "agents_told.json";
    std::vector<std::string> told;
    try {
        const Json given = core::parse_python_json(storage::read_file(marker), nullptr, core::ParseOptions{false, true});
        for (const Json& v : core::iterate(given)) {
            core::require_hashable(v);
            const std::string s = py_str(v);
            if (std::find(told.begin(), told.end(), s) == told.end()) told.push_back(s);
        }
    } catch (const core::PyTypeError&) {
        throw;
    } catch (const core::Error&) {  // (OSError, ValueError: told nothing yet)
        told.clear();
    }
    const std::string who = actor_.substr(0, actor_.find('/'));
    if (std::find(told.begin(), told.end(), who) == told.end()) {
        told.push_back(who);
        std::sort(told.begin(), told.end());
        Json list = Json::array();
        for (const std::string& t : told) list.push_back(t);
        try {
            fs::create_directories(marker.parent_path());
            storage::write_atomic(marker, studio::text_mode(core::dump_python(list, true)));
        } catch (const std::exception&) {
        }
        return request;
    }
    Json out = Json::object();
    for (const auto& [key, value] : request.items()) {
        if (key != "order_of_instructions" && key != "references_note") out[key] = value;
    }
    Json notes = Json::array();
    for (const Json& n : core::iterate(core::py_or(py_get(request, "notes_for_agent"), Json::array()))) {
        if (!(n == kStandingNotes[0]) && !(n == kStandingNotes[1])) notes.push_back(n);
    }
    out["notes_for_agent"] = std::move(notes);
    out["standing_notes"] = "最初の依頼と同じ注意（参照画像の順・構図は参考・文字を描かせない）は省いた。全部は dir の request.json にある";
    return out;
}

ToolResult StudioService::generation_request(const Json& a) {
    const fs::path path = project_path(a["project"]);
    Json purpose = a["purpose"];
    if (purpose.is_null()) purpose = py_truthy(a["character_id"]) ? "character_sheet" : py_truthy(a["location_id"]) ? "location" : "panel_art";
    const core::Document doc = load_episode(path);
    const Json& page = a["page"];
    if (core::is_one_of(purpose, {"panel_art", "draft"})) {
        const auto at = page_at(doc, page);
        if (!at) return fail(py_str(page) + " ページはない", "no_page", "/page");
        if (purpose == "panel_art" && !doc.page(*at).name_ok) {
            return fail(py_str(page) + " ページのネームが承認されていない（絵の依頼は承認後。ラフは purpose=draft）", "name_not_approved", "/page");
        }
    }
    genreq::Pack pack;
    try {
        genreq::Options o;
        o.purpose = purpose;
        o.mode = a["mode"];
        o.page = page;
        o.frame_id = a["frame_id"];
        o.character_id = a["character_id"];
        o.location_id = a["location_id"];
        o.parent = a["parent"];
        o.tool = a["tool"];
        o.instruction = a["instruction"];
        o.regions = a["regions"];
        o.focus_character = a["focus_character"];
        pack = genreq::build(doc, path, o);
    } catch (const genreq::RequestError& exc) {
        return fail(exc.what(), "bad_request", exc.path());
    }
    const fs::path folder = genreq::write(pack, path);
    {
        Session session(path, actor_);
        const Json requests = core::py_or(py_get(session.doc().studio, "requests"), Json::object());
        const std::string id = pack.id();
        const bool known = requests.is_object() && requests.contains(id);
        if (!known || !core::py_equals(py_get(requests[id], "status"), Json("open"))) {
            // (asked again after its images came in: another round of the same request)
            session.apply(Json::array({Json::object({{"op", "open_request"}, {"request", genreq::record(pack)}})}), actor_);
            session.save(actor_);
        }
    }
    Json files = Json::object();
    for (const auto& [name, data] : pack.files) {
        (void)data;
        files[name] = util::path_text(folder / core::path_from_utf8(name));
    }
    const auto sheet = guide_sheet(pack);
    std::vector<std::string> images;
    if (sheet) images.push_back(*sheet);
    return ToolResult(true, Json::object({{"request", told_once(path, pack.request)},
                                          {"dir", util::path_text(folder)},
                                          {"files", std::move(files)},
                                          {"inbox", util::path_text(path / "studio" / "inbox" / core::path_from_utf8(pack.id()))}}),
                      {}, std::move(images));
}

ToolResult StudioService::import_images(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& request_id = a["request_id"];
    const auto request = studio::read_request(path, py_str(request_id));
    if (!request) return fail("依頼 " + py_str(request_id) + " はない（generation_request で作る）", "no_request", "/request_id");
    Json items;
    std::optional<render::Image> first;
    try {
        std::tie(items, first) = importer::prepare(path, *request, a["images"]);
    } catch (const importer::ImportError& exc) {
        return fail(exc.what(), "import_failed", exc.path());
    }
    core::Document doc;
    {
        Session session(path, actor_);
        const Json requests = core::py_or(py_get(session.doc().studio, "requests"), Json::object());
        core::require_hashable(request_id);
        if (!(request_id.is_string() && requests.is_object() && requests.contains(request_id.get<std::string>()))) {
            session.apply(Json::array({Json::object({{"op", "open_request"}, {"request", record_from_request(*request)}})}), actor_);
        }
        session.apply(Json::array({Json::object({{"op", "import_candidates"}, {"request_id", request_id}, {"candidates", items}})}), actor_);
        session.save(actor_);
        doc = session.doc();
    }
    Json ids = Json::array();
    Json metrics = Json::object();
    for (const Json& i : items) {
        ids.push_back(i["id"]);
        metrics[i["id"].get<std::string>()] = i["metrics"];
    }
    std::vector<std::string> shown;
    const Json target = util::or_dict(py_get(*request, "target"));
    if (py_truthy(a["preview"]) && first && core::truthy_at(target, "frame_id")) {
        try {
            shown.push_back(render_kind(doc, path, Json(core::to_int(util::item(target, "page"))), target["frame_id"], Json("compare"), ids[0], Json(512)).first);
        } catch (const core::ApplyError&) {
        }
    }
    return ToolResult(true, Json::object({{"request_id", request_id}, {"candidates", std::move(ids)}, {"metrics", std::move(metrics)}}), {}, std::move(shown));
}

ToolResult StudioService::candidates(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const Json& character_id = a["character_id"];
    const Json& location_id = a["location_id"];
    std::vector<Json> items;
    Json adopted = Json::object();
    if (py_truthy(character_id) || py_truthy(location_id)) {
        const char* bucket = py_truthy(character_id) ? "character_candidates" : "location_candidates";
        const Json owner = py_truthy(character_id) ? character_id : location_id;
        const Json all = util::or_dict(py_get(doc.studio, bucket));
        core::require_hashable(owner);
        if (owner.is_string() && all.contains(owner.get<std::string>())) items = core::iterate(all[owner.get<std::string>()]);
    } else {
        const auto at = page_at(doc, a["page"]);
        if (!at) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
        const core::Page& target = doc.page(*at);
        Json panel;
        try {
            const Json wanted_frame(py_str(a["frame_id"]));
            const core::Frame& frame = briefs::find_frame(target, wanted_frame);
            panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
        } catch (const core::PyUncaught& e) {
            if (e.type() != "KeyError" && e.type() != "IndexError") throw;
            std::string ids;
            for (const core::Frame* f : target.leaf_frames()) ids += (ids.empty() ? "" : ", ") + f->id;
            return fail("コマ " + py_str(a["frame_id"]) + " はない（" + py_str(a["page"]) + " ページのコマ: " + ids + "）", "no_frame", "/frame_id");
        }
        items = core::iterate(py_get(panel, "candidates", Json::array()));
        adopted = core::py_or(py_get(panel, "adopted"), Json::object());
    }
    std::vector<Json> rows;
    for (const Json& cand : items) {
        const Json origin = util::or_dict(py_get(cand, "origin"));
        const Json id = util::item(cand, "id");
        Json as = Json::array();
        for (const auto& [key, value] : adopted.items()) {
            if (core::py_equals(value, id)) as.push_back(key);
        }
        Json shown_origin = Json::object();
        for (const char* key : {"kind", "tool_id", "model"}) {
            if (core::truthy_at(origin, key)) shown_origin[key] = origin[key];
        }
        rows.push_back(Json::object({{"id", id}, {"status", py_get(cand, "status")}, {"adopted_as", std::move(as)}, {"px", py_get(cand, "px")},
                                     {"mode", py_get(cand, "mode")}, {"parent", py_get(cand, "parent")}, {"request", py_get(cand, "request")},
                                     {"stale", py_get(cand, "stale", Json(false))}, {"review", py_get(cand, "review")},
                                     {"metrics", py_get(cand, "metrics")}, {"origin", std::move(shown_origin)}}));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Json& x, const Json& y) {
        const Json rx = py_get(core::py_or(x["metrics"], Json::object()), "rank", Json(0));
        const Json ry = py_get(core::py_or(y["metrics"], Json::object()), "rank", Json(0));
        if (core::py_less(rx, ry)) return true;
        if (core::py_less(ry, rx)) return false;
        return core::py_less(x["id"], y["id"]);
    });
    const auto sheet = contact_sheet(path, items);
    std::vector<std::string> images;
    if (sheet) images.push_back(*sheet);
    return ToolResult(true, Json::object({{"candidates", Json(rows)}}), {}, std::move(images));
}

ToolResult StudioService::review_candidates(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    std::vector<Json> cast;
    if (const auto at = page_at(doc, a["page"])) {
        try {
            const Json wanted_frame(py_str(a["frame_id"]));
            const core::Frame& frame = briefs::find_frame(doc.page(*at), wanted_frame);
            const Json panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
            for (const Json& c : core::iterate(py_get(panel, "characters", Json::array()))) {
                if (c.is_object()) cast.push_back(c);
            }
        } catch (const core::PyUncaught& e) {
            if (e.type() != "KeyError" && e.type() != "IndexError") throw;
        }
    }
    const std::vector<Json> reviews = core::iterate(core::py_or(a["reviews"], Json::array()));
    for (std::size_t i = 0; i < reviews.size(); ++i) {
        const Json checks = reviews[i].is_object() ? py_get(reviews[i], "checks") : Json();
        if (cast.empty()) continue;
        const std::string where = "/reviews/" + std::to_string(i) + "/checks";
        if (!checks.is_object()) {
            return fail("人物のいるコマの評価には checks {likeness（設定画に似ているか 0〜1）, hands（ok / broken / none）, "
                        "text（絵の中の文字 none / some）, cut（顔・手・頭が枠で切れる none / some）} が要る",
                        "checks_required", where);
        }
        const std::pair<const char*, std::vector<std::string>> wanted[] = {
            {"likeness", {}}, {"hands", {"ok", "broken", "none"}}, {"text", {"none", "some"}}, {"cut", {"none", "some"}}};
        for (const auto& [key, allowed] : wanted) {
            const Json value = py_get(checks, key);
            bool good = false;
            if (allowed.empty()) {
                good = value.is_number() || value.is_boolean() ? (0 <= core::to_float(value) && core::to_float(value) <= 1) : false;
            } else {
                good = value.is_string() && std::find(allowed.begin(), allowed.end(), value.get<std::string>()) != allowed.end();
            }
            if (!good) {
                std::string listed;
                for (const std::string& v : allowed) listed += (listed.empty() ? "" : " / ") + v;
                return fail(std::string("checks.") + key + " が無いか値が違う（" + (allowed.empty() ? std::string("0〜1") : listed) + "）", "checks_invalid",
                            where + "/" + key);
            }
        }
    }
    return ops(a["project"], Json::array({Json::object({{"op", "review_candidates"}, {"page", a["page"]}, {"frame_id", a["frame_id"]}, {"reviews", a["reviews"]}})}));
}

ToolResult StudioService::adopt(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& candidate_id = a["candidate_id"];
    const Json& location_id = a["location_id"];
    if (py_truthy(location_id)) {
        const core::Document doc = load_episode(path);
        Json cand;
        const Json all = util::or_dict(py_get(doc.studio, "location_candidates"));
        core::require_hashable(location_id);
        if (location_id.is_string() && all.contains(location_id.get<std::string>())) {
            for (const Json& c : core::iterate(all[location_id.get<std::string>()])) {
                if (core::py_equals(util::item(c, "id"), candidate_id)) {
                    cand = c;
                    break;
                }
            }
        }
        if (cand.is_null()) return fail("場所 " + py_str(location_id) + " の候補 " + py_str(candidate_id) + " はない", "no_candidate", "/candidate_id");
        return ops(a["project"], Json::array({Json::object({{"op", "attach_reference"}, {"target", Json::object({{"location_id", location_id}})},
                                                             {"asset", util::item(cand, "asset")}, {"kind", "reference"}})}));
    }
    Json page = a["page"];
    Json frame_id = a["frame_id"];
    if (page.is_null() || !py_truthy(frame_id)) {  // (the candidate says where it belongs)
        const core::Document doc = load_episode(path);
        bool found = false;
        for (const auto& pg : doc.pages) {
            for (const core::Frame* fr : pg->leaf_frames()) {
                const Json panel = fr->panel && py_truthy(*fr->panel) ? *fr->panel : Json::object();
                for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) {
                    if (core::py_equals(py_get(c, "id"), candidate_id)) found = true;
                }
                if (found) {
                    page = pg->index.json();
                    frame_id = fr->id;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return fail("候補 " + py_str(candidate_id) + " はどのコマにも無い", "no_candidate", "/candidate_id");
    }
    const Json& to = a["to"];
    Json op = Json::object({{"op", "adopt_candidate"}, {"page", page}, {"frame_id", frame_id}, {"candidate_id", candidate_id}, {"to", to}});
    for (const char* key : {"fit", "offset_mm", "scale"}) {
        if (!a[key].is_null()) op[key] = a[key];
    }
    ToolResult result = ops(a["project"], Json::array({op}));
    if (!result.ok) return result;
    Json steps = Json::array({Json::object({{"step", "adopt"}, {"ok", true}, {"candidate_id", candidate_id}})});
    Json final_id = candidate_id;
    Json dpi = Json::object();
    std::vector<Issue> extra;
    Json now;
    double wanted = 0.0;
    bool has_wanted = false;
    const auto stop = [&steps](const char* step, const ToolResult& failed) {
        steps.push_back(Json::object({{"step", step}, {"ok", false}}));
        Json data = Json::object({{"steps", steps}, {"stopped_at", step}});
        for (const auto& [key, value] : failed.data.items()) data[key] = value;
        return ToolResult(false, std::move(data), failed.issues);
    };
    const bool upscale_on = py_truthy(a["upscale"]);
    if (core::py_equals(to, Json("art"))) {  // (the resolution is told whether it is enlarged or not)
        const core::Document doc = load_episode(path);
        // (the page another writer took away since the candidate was adopted: Python's next() stops with StopIteration)
        const auto found = page_at(doc, page);
        if (!found) return fail(py_str(page) + " ページはない", "no_page", "/page");
        const core::Page& pg = doc.page(*found);
        const core::Layer* layer = nullptr;
        for (const core::Layer* la : preflight::art_layers(pg)) {
            const Json source = la->source && py_truthy(*la->source) ? *la->source : Json::object();
            if (core::py_equals(la->frame_id ? Json(*la->frame_id) : Json(), frame_id) && core::py_equals(py_get(source, "candidate"), candidate_id)) {
                layer = la;
                break;
            }
        }
        if (layer != nullptr) {
            if (const auto d = preflight::layer_dpi(pg, *layer, storage::AssetStore(path))) now = *d;
        }
        wanted = doc.spec.dpi.truthy() ? doc.spec.dpi.value() : 600.0;
        has_wanted = true;
        dpi = Json::object({{"dpi_before", now}, {"dpi_wanted", wanted}});
        if (upscale_on && py_truthy(now) && now.get<double>() < wanted * 0.95) {
            const ToolResult bigger = invoke("upscale", Json::object({{"project", a["project"]}, {"page", page}, {"frame_id", frame_id},
                                                                       {"candidate_id", candidate_id}, {"scale", Json()}, {"method", a["method"]}}));
            if (!bigger.ok) return stop("upscale", bigger);
            steps.push_back(Json::object({{"step", "upscale"}, {"ok", true}, {"candidate_id", bigger.data["candidate_id"]}, {"scale", py_get(bigger.data, "scale")}}));
            Json again_op = op;
            again_op["candidate_id"] = bigger.data["candidate_id"];
            const ToolResult again = ops(a["project"], Json::array({again_op}));
            if (!again.ok) return stop("adopt_upscaled", again);
            steps.push_back(Json::object({{"step", "adopt_upscaled"}, {"ok", true}, {"candidate_id", bigger.data["candidate_id"]}}));
            final_id = bigger.data["candidate_id"];
            dpi["dpi_after"] = py_get(bigger.data, "dpi_after");
        } else if (upscale_on) {
            steps.push_back(Json::object({{"step", "upscale"}, {"ok", true}, {"skipped", py_truthy(now) ? "解像度は足りている" : "解像度が分からない"}}));
        }
    }
    if (py_truthy(a["regions"])) {
        const ToolResult reported = invoke("report_regions", Json::object({{"project", a["project"]}, {"page", page}, {"frame_id", frame_id}, {"regions", a["regions"]}}));
        if (!reported.ok) return stop("report_regions", reported);
        steps.push_back(Json::object({{"step", "report_regions"}, {"ok", true}}));
    }
    const Json last = core::py_or(py_get(dpi, "dpi_after"), py_get(dpi, "dpi_before"));
    if (core::py_equals(to, Json("art")) && py_truthy(last) && has_wanted && wanted != 0.0 && core::to_float(last) < wanted * 0.95) {
        extra.push_back(warning("dpi_short", "/candidate_id",
                                "採用した絵は " + core::py_format_g(core::to_float(last)) + " dpi で、本の " + core::py_format_g(wanted) +
                                    " dpi に届いていない（印刷で粗く見えることがある）",
                                std::string("もっと大きい画像を作って取り込み直す（コマの縦横に合わせて描かせる）か、コマを小さくする") +
                                    (upscale_on ? "" : "。upscale: true で Genko が拡大する")));
    }
    Json data = Json::object({{"steps", steps}, {"page", page}, {"frame_id", frame_id}, {"candidate_id", candidate_id}, {"adopted", final_id},
                              {"upscaled", !core::py_equals(final_id, candidate_id)}});
    for (const auto& [key, value] : dpi.items()) data[key] = value;
    ToolResult out(true, std::move(data), joined(result.issues, extra));
    if (py_truthy(a["preview"])) {
        const ToolResult shown = invoke("render", Json::object({{"project", a["project"]}, {"page", page}, {"mode", core::py_equals(to, Json("draft")) ? "proof" : "print"},
                                                                 {"max_px", 512}, {"frame_id", frame_id}}));
        out.images = shown.images;
        out.files = shown.files;
        out.issues.insert(out.issues.end(), shown.issues.begin(), shown.issues.end());
    }
    return out;
}

std::variant<Json, ToolResult> StudioService::crop_inbox(const fs::path& path, const std::string& request_id, const Json& image, const Json& crop01) {
    std::vector<double> v;
    try {
        v = core::unpack_floats(crop01, 4);
    } catch (const core::Error&) {
        return fail("crop01 は [x, y, 幅, 高さ]（絵の中の 0..1）", "bad_crop", "/crop01");
    }
    const double x = v[0], y = v[1], w = v[2], h = v[3];
    if (!(0 <= x && x < 1 && 0 <= y && y < 1 && 0 < w && w <= 1 - x + 1e-6 && 0 < h && h <= 1 - y + 1e-6)) {
        return fail("crop01 は絵の中に収める（x, y は 0 以上、x+幅・y+高さは 1 まで）", "bad_crop", "/crop01");
    }
    std::string data;
    try {
        data = importer::read_source(path, image, 0);
    } catch (const importer::ImportError& exc) {
        return fail(exc.what(), "import_failed", exc.path());
    }
    const render::Image picture = open_picture(data);
    const auto r = [](double value) { return static_cast<int>(core::py_round_int(value)); };
    const render::Image cut = picture.crop(render::Box{r(x * picture.width()), r(y * picture.height()), r((x + w) * picture.width()), r((y + h) * picture.height())});
    const fs::path folder = importer::inbox(path) / core::path_from_utf8(request_id);
    fs::create_directories(folder);
    const std::string stem = stem_of(py_str(core::py_or(py_get(image, "file"), Json("asset"))));
    const fs::path target = folder / core::path_from_utf8(stem + "_crop.png");
    util::write_file(target, picture_png(cut));
    Json out = Json::object();
    for (const auto& [key, value] : image.items()) {
        if (key != "asset") out[key] = value;
    }
    out["file"] = core::path_to_utf8(target.lexically_relative(path));
    return out;
}

ToolResult StudioService::take_panel_art(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const Json& request_id = a["request_id"];
    const auto request = studio::read_request(path, py_str(request_id));
    if (!request) return fail("依頼 " + py_str(request_id) + " はない（generation_request で作る）", "no_request", "/request_id");
    const Json target = util::or_dict(py_get(*request, "target"));
    if (!core::is_one_of(py_get(*request, "purpose"), {"panel_art", "draft"}) || !core::truthy_at(target, "frame_id")) {
        return fail("take_panel_art はコマの依頼（panel_art・draft）だけ。設定画・場所は import_images と承認で", "not_a_panel", "/request_id");
    }
    const Json page(core::to_int(util::item(target, "page")));
    const Json frame_id(py_str(target["frame_id"]));
    const Json to = core::py_equals(py_get(*request, "purpose"), Json("draft")) ? Json("draft") : Json("art");
    Json steps = Json::array();
    const auto stop = [&steps](const char* step, const ToolResult& result) {
        steps.push_back(Json::object({{"step", step}, {"ok", false}}));
        Json data = Json::object({{"steps", steps}, {"stopped_at", step}});
        for (const auto& [key, value] : result.data.items()) data[key] = value;
        return ToolResult(false, std::move(data), result.issues, result.images, result.files);
    };
    Json image = a["image"];
    if (py_truthy(a["crop01"])) {
        auto cut = crop_inbox(path, py_str(request_id), image, a["crop01"]);
        if (auto* failed = std::get_if<ToolResult>(&cut)) return stop("crop", *failed);
        image = std::get<Json>(cut);
        steps.push_back(Json::object({{"step", "crop"}, {"ok", true}, {"file", image["file"]}}));
    }
    const ToolResult imported = invoke("import_images", Json::object({{"project", a["project"]}, {"request_id", request_id}, {"images", Json::array({image})},
                                                                      {"preview", false}}));
    if (!imported.ok) return stop("import", imported);
    const Json candidate = imported.data["candidates"][0];
    steps.push_back(Json::object({{"step", "import"}, {"ok", true}, {"candidate_id", candidate}}));
    const ToolResult adopted = invoke("adopt", Json::object({{"project", a["project"]}, {"candidate_id", candidate}, {"page", page}, {"frame_id", frame_id},
                                                             {"to", to}, {"regions", a["regions"]}, {"upscale", a["upscale"]}, {"method", a["method"]}}));
    if (!adopted.ok) {
        const Json theirs = core::py_or(py_get(adopted.data, "steps"), Json::array({Json::object({{"step", "adopt"}, {"ok", false}})}));
        for (const Json& s : core::iterate(theirs)) steps.push_back(s);
        Json data = adopted.data;
        data["steps"] = steps;
        data["stopped_at"] = py_get(adopted.data, "stopped_at", Json("adopt"));
        return ToolResult(false, std::move(data), adopted.issues);
    }
    for (const Json& s : core::iterate(adopted.data["steps"])) steps.push_back(s);
    Json data = Json::object({{"steps", steps}});
    for (const auto& [key, value] : adopted.data.items()) {
        if (key != "steps") data[key] = value;
    }
    data["note"] = "候補の点検（review_candidates）と人の承認は別。作画の承認の後に採用し直すと、承認は取り直しになる";
    return ToolResult(true, std::move(data), joined(imported.issues, adopted.issues), adopted.images, adopted.files);
}

ToolResult StudioService::report_regions(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto at = page_at(doc, a["page"]);
    if (!at) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
    const core::Page& target = doc.page(*at);
    const core::Layer* layer = nullptr;
    for (const core::Layer& lay : target.layers) {
        const Json source = lay.source && py_truthy(*lay.source) ? *lay.source : Json::object();
        if (lay.kind == core::LayerKind::Placed && core::py_equals(lay.frame_id ? Json(*lay.frame_id) : Json(), a["frame_id"]) &&
            core::py_equals(py_get(source, "to", Json("art")), Json("art"))) {
            layer = &lay;
            break;
        }
    }
    Json converted = Json::array();
    const std::vector<Json> regions = core::iterate(core::py_or(a["regions"], Json::array()));
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const Json& region = regions[i];
        const std::string where = "/regions/" + std::to_string(i);
        if (!region.is_object() || !core::truthy_at(region, "kind")) return fail("region は {kind, rect_mm | box01, char?}", "bad_region", where);
        Json item = Json::object();
        for (const char* key : {"kind", "char", "confidence"}) {
            if (!py_get(region, key).is_null()) item[key] = region[key];
        }
        if (core::truthy_at(region, "rect_mm")) {
            Json rect = Json::array();
            for (const Json& v : core::iterate(region["rect_mm"])) rect.push_back(core::py_round(core::to_float(v), 2));
            item["rect_mm"] = std::move(rect);
        } else if (core::truthy_at(region, "box01")) {
            if (layer == nullptr || !layer->placement_mm) return fail("box01 は採用した絵に対する座標。先に adopt する", "no_art", where);
            std::vector<double> b;
            for (const Json& v : core::iterate(region["box01"])) b.push_back(core::to_float(v));
            if (b.size() < 4) core::raise_index_error();
            const core::Rect& pl = *layer->placement_mm;
            item["rect_mm"] = Json::array({core::py_round(pl.x.value() + b[0] * pl.width.value(), 2), core::py_round(pl.y.value() + b[1] * pl.height.value(), 2),
                                           core::py_round(b[2] * pl.width.value(), 2), core::py_round(b[3] * pl.height.value(), 2)});
        } else {
            return fail("rect_mm か box01 が要る", "bad_region", where);
        }
        converted.push_back(std::move(item));
    }
    ToolResult result = ops(a["project"], Json::array({Json::object({{"op", "replace_regions"}, {"page", a["page"]}, {"frame_id", a["frame_id"]},
                                                                     {"source", "agent"}, {"regions", converted}})}));
    if (result.ok) {
        const core::Document after = load_episode(path);
        const auto found = page_at(after, a["page"]);  // (gone since the regions were saved: Python's StopIteration)
        if (!found) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
        const auto [plan_ops, notes] = finish::plan(after, after.page(*found));
        (void)plan_ops;
        Json preview_notes = Json::array();
        for (const Json& n : notes) {
            if (core::py_equals(py_get(n, "frame_id"), a["frame_id"])) preview_notes.push_back(n);
        }
        result.data["finish_preview"] = std::move(preview_notes);
    }
    return result;
}

ToolResult StudioService::finish_page(const Json& a) {
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto at = page_at(doc, a["page"]);
    if (!at) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
    if (!doc.page(*at).art_ok) return fail(py_str(a["page"]) + " ページの作画が承認されていない", "art_not_approved", "/page");
    auto [plan_ops, notes] = finish::plan(doc, doc.page(*at));
    Work work{doc, Json::array()};
    try {
        work.apply(plan_ops, actor_);
    } catch (const core::ApplyError& exc) {
        return fail(exc.what(), "apply_failed", "/");
    }
    const std::string bytes = preview(work.doc, a["page"], "proof", Json(1024), Json());
    Json data = Json::object({{"committed", false}, {"page", a["page"]}, {"changes", notes}, {"ops", static_cast<std::int64_t>(plan_ops.size())}});
    if (py_truthy(a["commit"])) {
        Session session(path, actor_);
        const auto found = page_at(session.doc(), a["page"]);  // (gone since it was read: Python's StopIteration)
        if (!found) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
        const auto again = finish::plan(session.doc(), session.doc().page(*found));
        session.apply(again.first, actor_);
        session.save(actor_);
        data["committed"] = true;
        data["changes"] = again.second;
    }
    return ToolResult(true, std::move(data), {}, {bytes});
}

ToolResult StudioService::derive(const Json& a) {
    const Json& kind = a["kind"];
    if (!core::py_equals(kind, Json("lineart"))) return fail("kind は lineart だけ", "bad_kind", "/kind");
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto at = page_at(doc, a["page"]);
    if (!at) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
    Json panel;
    try {
        const Json wanted_frame(py_str(a["frame_id"]));
        const core::Frame& frame = briefs::find_frame(doc.page(*at), wanted_frame);
        panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
    } catch (const core::PyUncaught& e) {
        if (e.type() != "KeyError" && e.type() != "IndexError") throw;
        return fail("コマ " + py_str(a["frame_id"]) + " はない", "no_frame", "/frame_id");
    }
    const Json source_id = py_truthy(a["candidate_id"]) ? a["candidate_id"] : py_get(util::or_dict(py_get(panel, "adopted")), "art");
    Json source;
    for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) {
        if (core::py_equals(py_get(c, "id"), source_id)) {
            source = c;
            break;
        }
    }
    if (source.is_null()) return fail("元の候補が無い（candidate_id を渡すか、先に採用する）", "no_candidate", "/candidate_id");
    storage::AssetStore store(path);
    const auto data = store.get_bytes(py_str(util::item(source, "asset")), ".png");
    if (!data) return fail("候補 " + py_str(source_id) + " の画像が assets/ に無い", "asset_missing", "/candidate_id");
    const Json params = util::or_dict(a["params"]);
    const Json settings = render::filters::line_settings(params);
    const render::Image layer = render::filters::extract_lines(open_picture(*data), params);
    const std::string ref = store.put_bytes(formats::png_bytes(layer), ".png");
    const std::string cand_id = "cd_" + studio::content_hash(Json::object({{"derive", kind}, {"from", source["asset"]}, {"params", settings}})).substr(7, 10);
    Json item = Json::object({{"id", cand_id}, {"asset", ref}, {"px", Json::array({layer.width(), layer.height()})}, {"mode", "derive"}, {"parent", source_id}});
    if (core::truthy_at(source, "mapping")) item["mapping"] = source["mapping"];  // (placed as its parent: lines on lines)
    item["origin"] = Json::object({{"kind", "genko"}, {"tool_id", "genko:" + py_str(kind)}, {"params", settings}});
    ToolResult result = ops(a["project"], Json::array({Json::object({{"op", "import_candidates"}, {"page", a["page"]}, {"frame_id", a["frame_id"]},
                                                                     {"candidates", Json::array({item})}})}));
    if (!result.ok) return result;
    render::Image shown = render::Image::create("RGB", layer.size(), render::Ink{255, 255, 255});
    shown.paste(layer, render::Point{0, 0}, &layer);
    shown.thumbnail(render::Size{512, 512});
    const std::vector<long> hist = layer.split()[3].histogram();
    double inked = 0;
    for (std::size_t i = 128; i < hist.size(); ++i) inked += static_cast<double>(hist[i]);
    const double ink_share = inked / std::max(1, layer.width() * layer.height());
    return ToolResult(true, Json::object({{"candidate", cand_id}, {"kind", kind}, {"parent", source_id}, {"ink_share", core::py_round(ink_share, 4)},
                                          {"adopt", Json::object({{"candidate_id", cand_id}, {"to", "ink"}})}}),
                      {}, {png(shown)});
}

ToolResult StudioService::upscale(const Json& a) {
    if (py_truthy(a["background"])) {
        return job(a["project"], "upscale", [a](StudioService& service) {
            Json now = a;
            now["background"] = false;
            return service.upscale(now);
        });
    }
    const fs::path path = project_path(a["project"]);
    const core::Document doc = load_episode(path);
    const auto at = page_at(doc, a["page"]);
    if (!at) return fail(py_str(a["page"]) + " ページはない", "no_page", "/page");
    const core::Page& target = doc.page(*at);
    Json panel;
    try {
        const Json wanted_frame(py_str(a["frame_id"]));
        const core::Frame& frame = briefs::find_frame(target, wanted_frame);
        panel = frame.panel && py_truthy(*frame.panel) ? *frame.panel : Json::object();
    } catch (const core::PyUncaught& e) {
        if (e.type() != "KeyError" && e.type() != "IndexError") throw;
        return fail("コマ " + py_str(a["frame_id"]) + " はない", "no_frame", "/frame_id");
    }
    const Json source_id = py_truthy(a["candidate_id"]) ? a["candidate_id"] : py_get(util::or_dict(py_get(panel, "adopted")), "art");
    Json source;
    for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) {
        if (core::py_equals(py_get(c, "id"), source_id)) {
            source = c;
            break;
        }
    }
    if (source.is_null()) return fail("元の候補が無い（candidate_id を渡すか、先に採用する）", "no_candidate", "/candidate_id");
    const Json& method = a["method"];
    core::require_hashable(method);
    if (!core::py_equals(method, Json(upscale::kBuiltin)) && !(method.is_string() && upscale::registered().contains(method.get<std::string>()))) {
        std::string names;
        for (const Json& u : upscale::available()) names += (names.empty() ? "" : ", ") + py_str(u["name"]);
        return fail("高解像度化の道具 " + py_str(method) + " は無い（使えるもの: " + names + "。道具の登録は人が genko studio upscaler add で行う）", "no_upscaler",
                    "/method");
    }
    storage::AssetStore store(path);
    const auto data = store.get_bytes(py_str(util::item(source, "asset")), ".png");
    if (!data) return fail("候補 " + py_str(source_id) + " の画像が assets/ に無い", "asset_missing", "/candidate_id");
    const double wanted = doc.spec.dpi.truthy() ? doc.spec.dpi.value() : 600.0;
    std::optional<double> now;
    for (const core::Layer* la : preflight::art_layers(target)) {
        const Json src = la->source && py_truthy(*la->source) ? *la->source : Json::object();
        if (core::py_equals(la->frame_id ? Json(*la->frame_id) : Json(), a["frame_id"]) && core::py_equals(py_get(src, "candidate"), source_id)) {
            now = preflight::layer_dpi(target, *la, store);
            break;
        }
    }
    Json scale = a["scale"];
    if (scale.is_null()) {
        if (!now || *now == 0.0) return fail("採用していない候補の倍率は決められない（scale を渡す）", "scale_required", "/scale");
        scale = core::py_min(upscale::kMaxScale, core::py_max(1.0, core::py_round(wanted / *now + 0.049, 1)));
    }
    const render::Image image = open_picture(*data);
    const double factor = core::to_float(scale);
    if (static_cast<double>(image.width()) * image.height() * (factor * factor) > static_cast<double>(kMaxImagePixels)) {
        return fail("拡大すると大きすぎる（" + std::to_string(image.width()) + "x" + std::to_string(image.height()) + " の " + py_str(scale) + " 倍）", "too_large",
                    "/scale");
    }
    render::Image big;
    try {
        big = upscale::enlarge(image, factor, py_str(method));
    } catch (const core::PyValueError& exc) {
        return fail(exc.what(), "upscale_failed", "/method");
    }
    const std::string ref = store.put_bytes(formats::png_bytes(big), ".png");
    const std::string cand_id = "cd_" + studio::content_hash(Json::object({{"upscale", method}, {"from", source["asset"]}, {"scale", scale}})).substr(7, 10);
    const Json marker = Json::object({{"from", source_id}, {"from_px", Json::array({image.width(), image.height()})}, {"scale", factor}, {"method", method}});
    Json item = Json::object({{"id", cand_id}, {"asset", ref}, {"px", Json::array({big.width(), big.height()})}, {"mode", "upscale"}, {"parent", source_id},
                              {"upscaled", marker}});
    if (core::truthy_at(source, "mapping")) item["mapping"] = source["mapping"];
    item["origin"] = Json::object({{"kind", "genko"}, {"tool_id", "genko:upscale:" + py_str(method)}, {"params", Json::object({{"scale", factor}})}});
    bool known = false;
    for (const Json& c : core::iterate(py_get(panel, "candidates", Json::array()))) known = known || core::py_equals(py_get(c, "id"), Json(cand_id));
    if (!known) {
        ToolResult result = ops(a["project"], Json::array({Json::object({{"op", "import_candidates"}, {"page", a["page"]}, {"frame_id", a["frame_id"]},
                                                                         {"candidates", Json::array({item})}})}));
        if (!result.ok) return result;
    }
    const Json after = now && *now != 0.0 ? Json(core::py_round(*now * factor, 1)) : Json();
    render::Image shown = big.convert("RGB");
    shown.thumbnail(render::Size{512, 512});
    return ToolResult(true, Json::object({{"candidate", cand_id}, {"candidate_id", cand_id}, {"parent", source_id}, {"px", Json::array({big.width(), big.height()})},
                                          {"scale", factor}, {"method", method}, {"dpi_before", now ? Json(*now) : Json()}, {"dpi_after", after},
                                          {"adopt", Json::object({{"candidate_id", cand_id}, {"page", a["page"]}, {"frame_id", a["frame_id"]}})},
                                          {"note", "拡大しただけで描き込みは増えない。作画の承認の後に採用すると、承認は取り直しになる"}}),
                      {}, {png(shown)});
}

ToolResult StudioService::import_image(const Json& a) {
    const fs::path project = project_path(a["project"]);
    const Json& given_path = a["path"];
    const Json& png_base64 = a["png_base64"];
    if (given_path.is_null() == png_base64.is_null()) return fail("path か png_base64 のどちらか一方を渡す", "image_source", "/");
    std::string blob;
    if (!given_path.is_null()) {
        if (!given_path.is_string()) throw core::PyTypeError("expected str, bytes or os.PathLike object, not " + core::py_type_name(given_path));
        const std::string text = given_path.get<std::string>();
        fs::path source = core::path_from_utf8(text);
        source = source.is_absolute() ? source : root_ / source;
        std::error_code ec;
        fs::path resolved = fs::weakly_canonical(source, ec);
        if (ec) resolved = source.lexically_normal();
        if (confine_) {
            bool under = false;
            auto p = resolved.begin();
            auto r = root_.begin();
            for (; r != root_.end() && p != resolved.end() && *p == *r; ++r, ++p) {
            }
            under = r == root_.end() && p != resolved.end();
            if (!under) return fail("path が --root の外: " + text, "outside_root", "/path");
        }
        if (!fs::is_regular_file(resolved, ec)) return fail("ファイルがない: " + text, "no_file", "/path");
        if (fs::file_size(resolved, ec) > kMaxImportBytes) return fail("画像が大きすぎる（64MB まで）", "too_large", "/path");
        blob = storage::read_file(resolved);
    } else {
        const Json text = core::py_or(png_base64, Json(""));
        if (!text.is_string()) throw core::PyTypeError("argument should be a bytes-like object or ASCII string, not '" + core::py_type_name(text) + "'");
        const auto decoded = b64decode_validate(text.get<std::string>());
        if (!decoded) return fail("png_base64 が base64 ではない", "bad_base64", "/png_base64");
        blob = *decoded;
    }
    try {
        render::verify_picture(blob);
    } catch (const core::OpError& exc) {
        throw core::ApplyError(exc.what());
    }
    const render::Image img = open_picture(blob);
    static const std::string kPng("\x89PNG\r\n\x1a\n", 8);
    const std::string stored = blob.compare(0, kPng.size(), kPng) == 0 ? blob : picture_png(img);
    const std::string ref = storage::AssetStore(project).put_bytes(stored, ".png");
    return ToolResult(true, Json::object({{"asset", ref}, {"px", Json::array({img.width(), img.height()})}, {"mode", std::string(img.mode())}}));
}

}  // namespace genko::service
