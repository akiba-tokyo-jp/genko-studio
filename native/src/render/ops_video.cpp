// PC5 J-e2 (video@1, SPEC.md PROD-01「ビデオレイヤー」): the video layers' ops — レイヤー → ビデオレイヤー ▸ and ラスタライズ ▸
// ビデオ, ファイル → 読み込み → ビデオフレームからレイヤー… — after PhotoCraft v0.2.0's crates/engine/src/video_cmds.rs (MIT,
// native/third_party/photocraft/NOTICE.md): a frame inserted, duplicated or deleted moves those after it, the timeline
// made longer where the row runs past it (edit_video). Genko's own where the upstream keeps no footage: a video layer is
// an animation folder's video row (core/video.hpp), an altered frame a paint layer (cel) in its folder, a frame restored
// shows its footage again (Photoshop's; the upstream clears it). The timeline's frame is the op's `at` (Genko's ops' frame
// is a panel). Not in `genko schema` or the MCP tool list, not an agent's (service::agent_op).

#include "render/ops_video.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "core/anim.hpp"
#include "core/color_raster.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/layer_basics.hpp"
#include "core/ops_util.hpp"
#include "core/production_json.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "core/video.hpp"
#include "render/codecs/codecs.hpp"
#include "render/footage.hpp"
#include "render/not_yet_ported.hpp"
#include "storage/asset_store.hpp"

namespace genko::render {

namespace {

using core::Document;
using core::Json;
using core::Layer;
using core::LayerKind;
using core::OpContext;
using core::OpError;
using core::Page;

constexpr std::int64_t kLayersMost = 500;  // (ビデオフレームからレイヤー: Photoshop's 500 frames)

std::string text_of(const Json& op, std::string_view key, std::size_t most, const std::string& what) {
    const Json* v = core::get(op, key);
    if (v == nullptr || v->is_null()) return {};
    if (!v->is_string()) throw OpError(what + " is text");
    std::string out = v->get<std::string>();
    if (out.size() > most) throw OpError(what + " is too long");
    return out;
}

// The animation of the page, made (12 fps, 24 frames, looping, as set_animation makes one) when it has none and `make`.
Json& anim_of(Page& page, bool make, std::optional<double> fps = std::nullopt) {
    if (!page.extra.is_object()) page.extra = Json::object();
    const auto found = page.extra.find("anim");
    if (found != page.extra.end() && found->is_object()) return *found;
    if (!make) throw OpError("page " + page.index.repr() + " is not an animation (set_animation first)");
    Json data = Json::object();
    data["fps"] = fps ? Json(*fps) : Json(12);
    data["frames"] = 24;
    data["loop"] = true;
    data["tracks"] = Json::array();
    page.extra["anim"] = std::move(data);
    return page.extra["anim"];
}

// The timeline made as long as `count` frames when it is shorter (at most 3000).
void lengthen(Json& anim, std::int64_t count) {
    if (count > core::anim::kMaxFrames) throw OpError("the video layer would be longer than " + std::to_string(core::anim::kMaxFrames) + " frames");
    const Json frames = anim.contains("frames") ? anim["frames"] : Json();
    std::int64_t now = 1;
    try {
        now = std::max<std::int64_t>(1, core::to_int(core::py_or(frames, 1)));
    } catch (const std::exception&) {
        throw OpError("the page's animation has frames that cannot be read");
    }
    if (count > now) anim["frames"] = count;
}

// The timeline's frames, at most 3000 (an anim before them may say more: the review's P2-2 — never laid frame by frame).
std::int64_t frames_of(const Json& anim) {
    try {
        return std::clamp<std::int64_t>(core::to_int(core::py_or(anim.contains("frames") ? anim.at("frames") : Json(), 1)), 1, core::anim::kMaxFrames);
    } catch (const std::exception&) {
        throw OpError("the page's animation has frames that cannot be read");
    }
}

double fps_of(const Json& anim) {
    try {
        return core::to_float(core::py_or(anim.contains("fps") ? anim.at("fps") : Json(), 12));
    } catch (const std::exception&) {
        throw OpError("the page's animation has an fps that cannot be read");
    }
}

// The video layer op["folder"] names: its folder's place and its track (to change).
struct Row {
    std::size_t folder = 0;  // (in page.layers)
    std::string id;
    Json* track = nullptr;
    Json& video() const { return (*track)["video"]; }
};

Row row_of(Page& page, const Json& op, bool unlocked = true) {
    const std::string id = text_of(op, "folder", 256, "folder");
    const std::string wrong = "folder is a video layer's id (add_video_layer)";
    if (id.empty()) throw OpError(wrong);
    Json& anim = anim_of(page, false);
    const auto tracks = anim.find("tracks");
    if (tracks == anim.end() || !tracks->is_array()) throw OpError(wrong);
    Row row;
    row.id = id;
    for (Json& track : *tracks) {
        if (track.is_object() && core::py_equals(core::py_get(track, "folder"), Json(id))) {
            row.track = &track;
            break;
        }
    }
    if (row.track == nullptr || !core::is_video_row(*row.track)) throw OpError(wrong);
    const auto folder = std::find_if(page.layers.begin(), page.layers.end(), [&](const Layer& l) { return l.id == id; });
    if (folder == page.layers.end() || folder->kind != LayerKind::Folder) throw OpError(wrong);
    row.folder = static_cast<std::size_t>(folder - page.layers.begin());
    if (unlocked && (folder->locked || core::locked_all(page.layers, row.folder))) throw OpError("the layer " + id + " is locked");
    return row;
}

// op[key]: a frame of the timeline (1..frames).
std::int64_t frame_at(const Json& op, std::int64_t count, std::string_view key = "at") {
    const Json* v = core::get(op, key);
    const std::string wrong = std::string(key) + " is a frame of the timeline, 1 to " + std::to_string(count);
    if (v == nullptr || v->is_null()) throw OpError(wrong);
    // (a whole number only: the review's P3-10)
    if (!v->is_number_integer()) throw OpError(wrong);
    const std::int64_t frame = v->is_number_unsigned() ? (v->get<std::uint64_t>() > static_cast<std::uint64_t>(count) ? count + 1 : static_cast<std::int64_t>(v->get<std::uint64_t>()))
                                                       : v->get<std::int64_t>();
    if (frame < 1 || frame > count) throw OpError(wrong);
    return frame;
}

// The row's frames as long as `count` at least (blank frames after its end).
void pad(Json& frames, std::int64_t count) {
    while (static_cast<std::int64_t>(frames.size()) < count) frames.push_back(Json::array({nullptr, nullptr}));
}

std::size_t layer_at(const Page& page, const std::string& id) {
    for (std::size_t i = 0; i < page.layers.size(); ++i)
        if (page.layers[i].id == id) return i;
    return page.layers.size();
}

// The cels (altered frames) taken out of the page: their references let go (core::forget_layers).
void drop_cels(Document& doc, std::size_t at, const std::set<std::string>& cels) {
    if (cels.empty()) return;
    Page& page = doc.edit_page(at);
    for (const std::string& id : cels) {
        const std::size_t i = layer_at(page, id);
        if (i < page.layers.size() && (page.layers[i].locked || core::locked_all(page.layers, i))) throw OpError("the layer " + id + " is locked");
    }
    std::erase_if(page.layers, [&](const Layer& l) { return cels.contains(l.id); });
    core::forget_layers(doc, at, cels);
}

void needs_color_raster(Document& doc, const Layer& layer) {
    if (layer.color_raster && std::find(doc.features.begin(), doc.features.end(), core::kColorRasterFeature) == doc.features.end()) {
        doc.features.emplace_back(core::kColorRasterFeature);
    }
}

// The footage an op gives ({kind, name, frames, fps, size_px, depth, alpha?, par?, source?}) in its normal form, its
// frames held by the book (in memory or in its folder).
Json footage_given(const Document& doc, const Json& given) {
    if (!given.is_object()) throw OpError("footage is {kind, name, frames, fps, size_px, depth, alpha?, par?, source?}");
    Json out = Json::object();
    for (const char* key : {"kind", "name", "frames", "fps", "size_px", "depth"}) {
        if (!given.contains(key)) throw OpError(std::string("footage.") + key + " is missing");
        out[key] = given.at(key);
    }
    out["alpha"] = given.contains("alpha") ? given.at("alpha") : Json("straight");
    out["par"] = given.contains("par") ? given.at("par") : Json(1.0);
    if (given.contains("source") && !given.at("source").is_null()) out["source"] = given.at("source");
    for (const auto& [key, value] : given.items()) {
        if (!out.contains(key) && key != "source") throw OpError("footage." + key + " is not a footage's");
    }
    if (out["fps"].is_number()) out["fps"] = out["fps"].get<double>();
    if (out["par"].is_number()) out["par"] = out["par"].get<double>();
    try {
        core::check_video(Json{{"footage", out}, {"frames", Json::array()}, {"show_altered", true}, {"placement_mm", Json::array({0, 0, 1, 1})},
                               {"fit", "contain"}});
    } catch (const core::Error& error) {
        throw OpError(error.what());
    }
    std::set<std::string> seen;
    for (const Json& ref : out["frames"]) {
        const std::string& r = ref.get_ref<const std::string&>();
        if (!seen.insert(r).second) continue;
        const bool held = doc.smart_files.contains(r) || (doc.asset_dir && storage::AssetStore(*doc.asset_dir).has(r, core::kFootageSuffix));
        if (!held) throw OpError("the footage's frame " + r + " is not in the book");
    }
    // (its first frame's header as the footage says: its size and depth — the book's 4 GiB is counted by them)
    const std::optional<std::string> first = footage::frame_bytes(doc, out["frames"][0].get<std::string>());
    const auto size = first ? codecs::header_size(*first) : std::nullopt;
    const int depth = first && first->size() > 24 && first->compare(1, 3, "PNG") == 0 ? static_cast<unsigned char>((*first)[24]) : 0;
    if (!size || static_cast<std::int64_t>(size->first) != out["size_px"][0].get<std::int64_t>() ||
        static_cast<std::int64_t>(size->second) != out["size_px"][1].get<std::int64_t>() || depth != out["depth"].get<std::int64_t>()) {
        throw OpError("the footage's frames are not the PNG pictures of its size_px and depth");
    }
    return out;
}


// The book's footage, all the rows', within core::kFootageMaxBytes.
void check_footage_room(const Document& doc) {
    if (core::book_footage_bytes(doc) > core::kFootageMaxBytes) {
        throw OpError("the book's footage would take more than 4 GiB decoded (read part of it: range, every)");
    }
}

std::array<double, 4> placement_given(const Json& op, const Page& page) {
    const Json* v = core::get(op, "placement_mm");
    if (v == nullptr || v->is_null()) {
        const core::Rect trim = page.trim_rect_mm();
        return {trim.x.value(), trim.y.value(), trim.width.value(), trim.height.value()};
    }
    try {
        return core::production::rect_mm(*v, "placement_mm");
    } catch (const core::Error&) {
        throw OpError("placement_mm is [x, y, w, h] (mm)");
    }
}

std::string fit_given(const Json& op, const std::string& fallback) {
    const std::string fit = text_of(op, "fit", 16, "fit");
    if (fit.empty()) return fallback;
    if (fit != "contain" && fit != "cover" && fit != "stretch") throw OpError("fit is contain, cover or stretch");
    return fit;
}

// The footage's frames laid on the row from `start` (1..): their indices at the timeline's speed, in the row's frames
// from there (the altered cels where they are), the frames after the laid ones keeping their cels without footage.
void lay(Json& frames, const Json& footage, double timeline_fps, std::int64_t start) {
    const std::vector<std::int64_t> laid =
        core::laid_footage(static_cast<std::int64_t>(footage.at("frames").size()), footage.at("fps").get<double>(), timeline_fps);
    pad(frames, start - 1 + static_cast<std::int64_t>(laid.size()));
    for (std::size_t k = static_cast<std::size_t>(start - 1); k < frames.size(); ++k) {
        const std::size_t i = k - static_cast<std::size_t>(start - 1);
        frames[k][0] = i < laid.size() ? Json(laid[i]) : Json();
    }
}

// フッテージを解釈's new speed (the review's P3-11): the row's footage laid again at it where it runs as it was laid — each run
// of footage frames one after another (as the old speed laid them) re-laid from its first to its last at the new speed —,
// what was inserted (a blank frame) and the gaps a deletion left kept between the runs; the altered cels where they were.
void relay(Json& frames, double old_fps, double new_fps, double timeline_fps) {
    const auto step = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(old_fps / timeline_fps - 1e-9)));
    std::vector<Json> column;  // (the new footage numbers)
    std::size_t k = 0;
    while (k < frames.size()) {
        const Json& at = frames[k][0];
        if (!at.is_number_integer()) {
            column.emplace_back();
            ++k;
            continue;
        }
        const std::int64_t first = at.get<std::int64_t>();
        std::int64_t last = first;
        std::size_t j = k + 1;
        while (j < frames.size() && frames[j][0].is_number_integer() && frames[j][0].get<std::int64_t>() >= last &&
               frames[j][0].get<std::int64_t>() <= last + step) {
            last = frames[j][0].get<std::int64_t>();
            ++j;
        }
        for (const std::int64_t n : core::laid_footage(last - first + 1, new_fps, timeline_fps)) {
            column.emplace_back(first + n);
            if (static_cast<std::int64_t>(column.size()) > core::anim::kMaxFrames) break;
        }
        k = j;
    }
    std::size_t length = column.size();
    for (std::size_t i = 0; i < frames.size(); ++i)
        if (!frames[i][1].is_null()) length = std::max(length, i + 1);
    if (static_cast<std::int64_t>(length) > core::anim::kMaxFrames) throw OpError("the video layer would be longer than 3000 frames");
    Json out = Json::array();
    for (std::size_t i = 0; i < length; ++i) {
        out.push_back(Json::array({i < column.size() ? column[i] : Json(), i < frames.size() ? frames[i][1] : Json()}));
    }
    frames = std::move(out);
}

// A paint layer of the footage frame shown at that frame of the row (a cel's copy, the footage frame, or empty).
Layer frame_picture(const Document& doc, const Page& page, const Row& row, std::int64_t at) {
    const Json& video = row.video();
    const core::VideoShown shown = core::video_shown(page, Json(row.id), video, at);
    if (shown.kind == core::VideoShown::Kind::Cel) {
        Layer copy = page.layers[layer_at(page, shown.cel)];
        return copy;
    }
    if (shown.kind == core::VideoShown::Kind::Footage) {
        try {
            return footage::footage_layer(doc, page, video, shown.index);
        } catch (const core::OpError&) {
            throw;
        } catch (const NotYetPorted&) {
            throw;
        } catch (const core::Error& error) {
            throw OpError(std::string("the footage's frame cannot be drawn (") + error.what() + ")");
        }
    }
    Layer empty;
    empty.role = core::LayerRole::User;
    empty.kind = LayerKind::Raster;
    empty.exportable = true;
    return empty;
}

// --- add_video_layer -------------------------------------------------------------------------------------------------------

// add_video_layer {page, footage?: {…} | null, name?, id?, at?: frame, placement_mm?, fit?} → {folder, frames}: レイヤー →
// ビデオレイヤー → ファイルから新規ビデオレイヤー… (footage) and 新規空白ビデオレイヤー (none: a blank frame for each frame of the
// timeline). The page made an animation (at the footage's speed) when it is not one, the timeline made as long as the row.
void add_video_layer(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t at = core::require_page(doc, c.op);
    const Json* given = core::get(c.op, "footage");
    const std::optional<Json> footage = given != nullptr && !given->is_null() ? std::optional<Json>(footage_given(doc, *given)) : std::nullopt;
    const std::array<double, 4> placement = placement_given(c.op, doc.page(at));
    const std::string fit = fit_given(c.op, "contain");
    const std::string name = text_of(c.op, "name", 1024, "name");
    const std::string wanted = text_of(c.op, "id", 256, "id");
    Page& page = doc.edit_page(at);
    std::optional<double> speed;
    if (footage) {
        const double f = (*footage)["fps"].get<double>();
        if (f >= core::kTimelineFpsMin && f <= core::kTimelineFpsMax) speed = f;
    }
    const bool made = !core::anim::is_animation(page);
    Json& anim = anim_of(page, true, speed);
    if (!anim.contains("tracks")) anim["tracks"] = Json::array();
    if (!anim["tracks"].is_array()) throw OpError("the page's animation has tracks that cannot be read");
    const std::int64_t count = made && footage ? 1 : frames_of(anim);
    const std::int64_t start = core::has(c.op, "at") ? frame_at(c.op, core::anim::kMaxFrames) : 1;
    Json frames = Json::array();
    if (footage) {
        lay(frames, *footage, fps_of(anim), start);
    } else {
        pad(frames, std::max(count, start));
    }
    if (made && footage) anim["frames"] = 1;  // (as long as the footage: below)
    lengthen(anim, static_cast<std::int64_t>(frames.size()));
    Layer folder;
    folder.id = wanted.empty() ? core::new_id() : wanted;
    if (layer_at(page, folder.id) < page.layers.size()) throw OpError("layer " + folder.id + " exists");
    folder.role = core::LayerRole::User;
    folder.kind = LayerKind::Folder;
    std::int64_t videos = 1;
    for (const Json& track : anim["tracks"]) videos += core::is_video_row(track) ? 1 : 0;
    std::string title = name;
    if (title.empty() && footage) {
        title = (*footage)["name"].get<std::string>();
        if (const auto dot = title.rfind('.'); dot != std::string::npos && dot > 0) title.resize(dot);
    }
    folder.title = title.empty() ? "ビデオレイヤー " + std::to_string(videos) : title;
    folder.exportable = true;
    const std::string id = folder.id;
    page.layers.push_back(std::move(folder));
    Json video = Json::object();
    video["footage"] = footage ? *footage : Json();
    video["frames"] = std::move(frames);
    video["show_altered"] = true;
    video["placement_mm"] = core::production::to_json(placement);
    video["fit"] = fit;
    const std::size_t length = video["frames"].size();
    anim["tracks"].push_back(Json{{"folder", id}, {"cels", Json::array()}, {"video", std::move(video)}});
    check_footage_room(doc);
    c.report = Json{{"folder", id}, {"frames", length}};
}

// --- the frames ------------------------------------------------------------------------------------------------------------

// video_insert_blank {page, folder, at}: 空白フレームを挿入 — a blank frame at `at`, those from it one later.
void video_insert_blank(OpContext& c) {
    Page& page = c.doc.edit_page(core::require_page(c.doc, c.op));
    const Row row = row_of(page, c.op);
    Json& anim = anim_of(page, false);
    const std::int64_t at = frame_at(c.op, frames_of(anim));
    Json& frames = row.video()["frames"];
    pad(frames, at - 1);
    frames.insert(frames.begin() + (at - 1), Json::array({nullptr, nullptr}));
    lengthen(anim, static_cast<std::int64_t>(frames.size()));
    c.report = Json{{"frames", frames.size()}};
}

// video_duplicate {page, folder, at}: フレームを複製 — the frame at `at` again after it (its altered cel copied), those after
// it one later.
void video_duplicate(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    Json& anim = anim_of(page, false);
    const std::int64_t at = frame_at(c.op, frames_of(anim));
    Json& frames = row.video()["frames"];
    pad(frames, at);
    Json copy = frames[static_cast<std::size_t>(at - 1)];
    if (copy[1].is_string()) {
        const std::size_t i = layer_at(page, copy[1].get<std::string>());
        if (i < page.layers.size()) {
            Layer cel = page.layers[i];
            cel.id = core::new_id();
            cel.title = "フレーム " + std::to_string(at + 1);
            cel.native_psd.reset();
            copy[1] = cel.id;
            page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(i), std::move(cel));
        } else {
            copy[1] = nullptr;
        }
    }
    Json& again = row_of(page, c.op).video()["frames"];  // (the page's layers moved: the row again)
    again.insert(again.begin() + at, std::move(copy));
    lengthen(anim_of(page, false), static_cast<std::int64_t>(again.size()));
    c.report = Json{{"frames", again.size()}};
}

// video_delete {page, folder, at}: フレームを削除 — the frame at `at` taken out (its altered cel with it), those after it one
// earlier (the timeline as long as it was).
void video_delete(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    const std::int64_t at = frame_at(c.op, frames_of(anim_of(page, false)));
    Json& frames = row.video()["frames"];
    std::set<std::string> gone;
    if (at <= static_cast<std::int64_t>(frames.size())) {
        if (frames[static_cast<std::size_t>(at - 1)][1].is_string()) gone.insert(frames[static_cast<std::size_t>(at - 1)][1].get<std::string>());
        frames.erase(frames.begin() + (at - 1));
    }
    const std::size_t left = frames.size();
    drop_cels(doc, p, gone);
    c.report = Json{{"frames", left}};
}

// video_restore {page, folder, at}: フレームを復元 — the altered frame at `at` shows its footage again (its cel taken out).
void video_restore(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    const std::int64_t at = frame_at(c.op, frames_of(anim_of(page, false)));
    Json& frames = row.video()["frames"];
    std::set<std::string> gone;
    if (at <= static_cast<std::int64_t>(frames.size()) && frames[static_cast<std::size_t>(at - 1)][1].is_string()) {
        gone.insert(frames[static_cast<std::size_t>(at - 1)][1].get<std::string>());
        frames[static_cast<std::size_t>(at - 1)][1] = nullptr;
    }
    drop_cels(doc, p, gone);
    c.report = Json{{"restored", gone.size()}};
}

// video_restore_all {page, folder}: すべてのフレームを復元 — every altered frame shows its footage again.
void video_restore_all(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    std::set<std::string> gone;
    for (Json& frame : row.video()["frames"]) {
        if (frame[1].is_string()) gone.insert(frame[1].get<std::string>());
        frame[1] = nullptr;
    }
    drop_cels(doc, p, gone);
    c.report = Json{{"restored", gone.size()}};
}

// video_alter {page, folder, at} → {cel, made}: the frame at `at` altered — its cel (a paint layer in the folder holding the
// footage frame's pixels, or empty on a blank frame) made when it has none, 変更されたビデオを表示 put on. What a stroke on a
// video layer begins with (the window); drawn on, the cel is the frame.
void video_alter(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    {
        Page& page = doc.edit_page(p);
        const Row row = row_of(page, c.op);
        const std::int64_t at = frame_at(c.op, frames_of(anim_of(page, false)));
        Json& frames = row.video()["frames"];
        pad(frames, at);
        const Json& now = frames[static_cast<std::size_t>(at - 1)][1];
        if (now.is_string() && layer_at(page, now.get<std::string>()) < page.layers.size()) {
            row.video()["show_altered"] = true;
            c.report = Json{{"cel", now}, {"made", false}};
            return;
        }
        frames[static_cast<std::size_t>(at - 1)][1] = nullptr;
    }
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    const std::int64_t at = frame_at(c.op, frames_of(anim_of(page, false)));
    Layer cel = frame_picture(doc, page, row, at);
    cel.id = core::new_id();
    cel.title = "フレーム " + std::to_string(at);
    cel.parent_id = row.id;
    cel.visible = true;
    cel.locked = false;
    cel.native_psd.reset();
    needs_color_raster(doc, cel);
    const std::string id = cel.id;
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(row_of(page, c.op).folder), std::move(cel));
    const Row again = row_of(page, c.op);
    again.video()["frames"][static_cast<std::size_t>(at - 1)][1] = id;
    again.video()["show_altered"] = true;
    c.report = Json{{"cel", id}, {"made", true}};
}

// --- the footage ---------------------------------------------------------------------------------------------------------

// set_video_layer {page, folder, show_altered?, fps?, alpha?, par?, placement_mm?, fit?}: 変更されたビデオを表示 and フッテージを
// 解釈… (the footage's speed — its frames laid again at the timeline's from where they begin, the altered frames where
// they are —, its alpha read straight, premultiplied or ignored, its pixels' aspect ratio) and where it is laid:
// placement_mm (null: the finished size) is the page's mm as it is now — the row's transform (the footage turned with its
// page, PC6) taken away with it, the footage laid upright in the box given (review of PC6-B: B2's P2-2).
void set_video_layer(OpContext& c) {
    Page& page = c.doc.edit_page(core::require_page(c.doc, c.op));
    const Row row = row_of(page, c.op);
    Json& video = row.video();
    for (const auto& [key, value] : c.op.items()) {
        static const std::set<std::string> kKeys{"op", "page", "folder", "show_altered", "fps", "alpha", "par", "placement_mm", "fit"};
        if (!kKeys.contains(key)) throw OpError("set_video_layer takes show_altered, fps, alpha, par, placement_mm, fit (not " + key + ")");
    }
    if (const Json* v = core::get(c.op, "show_altered"); v != nullptr && !v->is_null()) {
        if (!v->is_boolean()) throw OpError("show_altered is true or false");
        video["show_altered"] = v->get<bool>();
    }
    const bool footage_keys = core::has(c.op, "fps") || core::has(c.op, "alpha") || core::has(c.op, "par");
    if (footage_keys && video["footage"].is_null()) throw OpError("the video layer has no footage (a blank video layer)");
    if (const Json* v = core::get(c.op, "alpha"); v != nullptr) {
        if (!v->is_string() || (*v != Json("straight") && *v != Json("premultiplied") && *v != Json("ignore")))
            throw OpError("alpha is straight, premultiplied or ignore");
        video["footage"]["alpha"] = *v;
    }
    if (const Json* v = core::get(c.op, "par"); v != nullptr) {
        if (!v->is_number() || !std::isfinite(v->get<double>()) || v->get<double>() < 0.01 || v->get<double>() > 100) throw OpError("par is 0.01 to 100");
        video["footage"]["par"] = v->get<double>();
    }
    if (const Json* v = core::get(c.op, "fps"); v != nullptr) {
        double fps = 0;
        try {
            fps = core::to_float(*v);
        } catch (const std::exception&) {
            throw OpError("fps is 0.5 to 120");
        }
        if (!(fps >= core::kTimelineFpsMin && fps <= core::kTimelineFpsMax)) throw OpError("fps is 0.5 to 120");
        const double before = video["footage"]["fps"].get<double>();
        video["footage"]["fps"] = fps;
        Json& frames = video["frames"];
        relay(frames, before, fps, fps_of(anim_of(page, false)));
        lengthen(anim_of(page, false), static_cast<std::int64_t>(frames.size()));
    }
    if (core::has(c.op, "placement_mm")) {
        video["placement_mm"] = core::production::to_json(placement_given(c.op, page));
        video.erase("transform");  // (the box given the page's as it is now: upright in it)
    }
    if (core::has(c.op, "fit")) video["fit"] = fit_given(c.op, video["fit"].get<std::string>());
    c.report = Json{{"show_altered", video["show_altered"]}};
}

// replace_footage {page, folder, footage}: フッテージを置き換え… and フレームを再読み込み — the footage another (or the same
// file read again): each frame shows the new footage's frame of its number (those past its end nothing); a blank video
// layer's footage laid from its first frame.
void replace_footage(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    const Json* given = core::get(c.op, "footage");
    if (given == nullptr || given->is_null()) throw OpError("footage is {kind, name, frames, fps, size_px, depth, alpha?, par?, source?}");
    const Json footage = footage_given(doc, *given);
    Page& page = doc.edit_page(p);
    const Row row = row_of(page, c.op);
    Json& video = row.video();
    const bool blank = video["footage"].is_null();
    video["footage"] = footage;
    Json& frames = video["frames"];
    if (blank) {
        lay(frames, footage, fps_of(anim_of(page, false)), 1);
    } else {
        const auto count = static_cast<std::int64_t>(footage.at("frames").size());
        for (Json& frame : frames) {
            if (frame[0].is_number_integer() && frame[0].get<std::int64_t>() >= count) frame[0] = nullptr;
        }
    }
    lengthen(anim_of(page, false), static_cast<std::int64_t>(frames.size()));
    check_footage_room(doc);
    c.report = Json{{"frames", frames.size()}};
}

// rasterize_video {page, folder, at, id?} → {id}: ビデオレイヤー → ラスタライズ and ラスタライズ → ビデオ — what the video layer
// shows at `at` as an ordinary paint layer in its place; the video layer (its folder, its altered cels, its row) gone.
void rasterize_video(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    const std::string wanted = text_of(c.op, "id", 256, "id");
    Layer layer;
    std::string folder_id;
    {
        Page& page = doc.edit_page(p);
        const Row row = row_of(page, c.op);
        const std::int64_t at = frame_at(c.op, frames_of(anim_of(page, false)));
        layer = frame_picture(doc, doc.page(p), row, at);
        folder_id = row.id;
    }
    const Page& seen = doc.page(p);
    const std::size_t folder = layer_at(seen, folder_id);
    const Layer& was = seen.layers[folder];
    layer.id = wanted.empty() ? core::new_id() : wanted;
    if (layer_at(seen, layer.id) < seen.layers.size()) throw OpError("layer " + layer.id + " exists");
    layer.title = was.title;
    layer.parent_id = was.parent_id;
    layer.visible = was.visible;
    layer.locked = false;
    layer.lock_alpha = false;
    layer.native_psd.reset();
    std::set<std::string> gone{folder_id};
    for (const std::size_t i : core::folder_contents(seen.layers, folder)) {
        if (seen.layers[i].locked || core::locked_all(seen.layers, i)) throw OpError("the layer " + seen.layers[i].id + " is locked");
        gone.insert(seen.layers[i].id);
    }
    needs_color_raster(doc, layer);
    Page& page = doc.edit_page(p);
    const std::string id = layer.id;
    page.layers.insert(page.layers.begin() + static_cast<std::ptrdiff_t>(folder), std::move(layer));
    std::erase_if(page.layers, [&](const Layer& l) { return gone.contains(l.id); });
    core::forget_layers(doc, p, gone);
    c.report = Json{{"id", id}};
}

// footage_to_layers {page, footage, every?, range?: [a, b], into?: "layers" | "cels", placement_mm?, fit?, name?} → {ids} |
// {folder, cels}: ファイル → 読み込み → ビデオフレームからレイヤー… — each picture of the footage (a frame shown more than once,
// once; every n-th of [a, b]) a paint layer of its own (layers: on top, in order), or a cel of a new animation folder shown
// one frame each (cels: the page made an animation when it is not one). At most 500, as Photoshop's.
void footage_to_layers(OpContext& c) {
    Document& doc = c.doc;
    const std::size_t p = core::require_page(doc, c.op);
    const Json* given = core::get(c.op, "footage");
    if (given == nullptr || given->is_null()) throw OpError("footage is {kind, name, frames, fps, size_px, depth, alpha?, par?, source?}");
    const Json footage = footage_given(doc, *given);
    const std::string into = text_of(c.op, "into", 16, "into").empty() ? std::string("layers") : text_of(c.op, "into", 16, "into");
    if (into != "layers" && into != "cels") throw OpError("into is layers or cels");
    std::vector<std::string> pictures;  // (each picture once where it is shown more than once in a row)
    for (const Json& ref : footage.at("frames")) {
        if (pictures.empty() || pictures.back() != ref.get<std::string>()) pictures.push_back(ref.get<std::string>());
    }
    const auto count = static_cast<std::int64_t>(pictures.size());
    std::int64_t every = 1, first = 1, last = count;
    if (const Json* v = core::get(c.op, "every"); v != nullptr && !v->is_null()) {
        try {
            every = core::to_int_held(*v);
        } catch (const std::exception&) {
            every = 0;
        }
        if (every < 1 || every > core::anim::kMaxFrames) throw OpError("every is 1 to 3000");
    }
    if (const Json* v = core::get(c.op, "range"); v != nullptr && !v->is_null()) {
        const std::string wrong = "range is [first, last] of the footage's pictures 1 to " + std::to_string(count);
        if (!v->is_array() || v->size() != 2) throw OpError(wrong);
        try {
            first = core::to_int_held((*v)[0]);
            last = core::to_int_held((*v)[1]);
        } catch (const std::exception&) {
            throw OpError(wrong);
        }
        if (first < 1 || last > count || first > last) throw OpError(wrong);
    }
    std::vector<std::int64_t> picked;
    for (std::int64_t f = first; f <= last; f += every) picked.push_back(f);
    if (static_cast<std::int64_t>(picked.size()) > kLayersMost) {
        throw OpError("at most " + std::to_string(kLayersMost) + " frames become layers (every, range)");
    }
    const Json video{{"footage", footage}, {"frames", Json::array()}, {"show_altered", true},
                     {"placement_mm", core::production::to_json(placement_given(c.op, doc.page(p)))}, {"fit", fit_given(c.op, "contain")}};
    std::vector<Layer> made;
    for (const std::int64_t f : picked) {
        Json one = video;
        one["footage"]["frames"] = Json::array({pictures[static_cast<std::size_t>(f - 1)]});
        Layer layer;
        try {
            layer = footage::footage_layer(doc, doc.page(p), one, 0);
        } catch (const core::OpError&) {
            throw;
        } catch (const NotYetPorted&) {
            throw;
        } catch (const core::Error& error) {
            throw OpError(std::string("the footage's frame cannot be drawn (") + error.what() + ")");
        }
        layer.id = core::new_id();
        layer.title = "フレーム " + std::to_string(f);
        needs_color_raster(doc, layer);
        made.push_back(std::move(layer));
    }
    Page& page = doc.edit_page(p);
    if (into == "layers") {
        Json ids = Json::array();
        for (Layer& layer : made) {
            ids.push_back(layer.id);
            page.layers.push_back(std::move(layer));
        }
        c.report = Json{{"ids", ids}};
        return;
    }
    const double f = footage.at("fps").get<double>();
    Json& anim = anim_of(page, true, f >= core::kTimelineFpsMin && f <= core::kTimelineFpsMax ? std::optional<double>(f) : std::nullopt);
    if (!anim.contains("tracks")) anim["tracks"] = Json::array();
    if (!anim["tracks"].is_array()) throw OpError("the page's animation has tracks that cannot be read");
    lengthen(anim, static_cast<std::int64_t>(made.size()));
    Layer folder;
    folder.id = core::new_id();
    folder.role = core::LayerRole::User;
    folder.kind = LayerKind::Folder;
    std::string title = text_of(c.op, "name", 1024, "name");
    if (title.empty()) title = footage.at("name").get<std::string>();
    folder.title = title.empty() ? "アニメーション " + std::to_string(anim["tracks"].size() + 1) : title;
    folder.exportable = true;
    const std::string folder_id = folder.id;
    page.layers.push_back(std::move(folder));
    Json sheet = Json::array(), cels = Json::array();
    std::int64_t frame = 1;
    for (Layer& layer : made) {
        layer.parent_id = folder_id;
        sheet.push_back(Json::array({frame++, layer.id}));
        cels.push_back(layer.id);
        page.layers.insert(page.layers.end() - 1, std::move(layer));  // (a folder's layers sit just under it)
    }
    anim["tracks"].push_back(Json{{"folder", folder_id}, {"cels", std::move(sheet)}});
    c.report = Json{{"folder", folder_id}, {"cels", cels}};
}

}  // namespace

void register_video_ops(core::OpRegistry& registry) {
    registry.add("add_video_layer", add_video_layer);
    registry.add("video_insert_blank", video_insert_blank);
    registry.add("video_duplicate", video_duplicate);
    registry.add("video_delete", video_delete);
    registry.add("video_restore", video_restore);
    registry.add("video_restore_all", video_restore_all);
    registry.add("video_alter", video_alter);
    registry.add("set_video_layer", set_video_layer);
    registry.add("replace_footage", replace_footage);
    registry.add("rasterize_video", rasterize_video);
    registry.add("footage_to_layers", footage_to_layers);
}

}  // namespace genko::render
