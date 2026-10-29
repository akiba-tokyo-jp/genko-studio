// timeline@1 and video@1 (PC5 J-0): what the timeline and the video layers add inside a page's anim, and their checks
// (core/video.hpp). The work area after PhotoCraft v0.2.0's doc/src/video.rs Timeline (MIT,
// native/third_party/photocraft/NOTICE.md); the video row is Genko's own (an animation folder's).

#include "core/video.hpp"

#include <cmath>
#include <limits>

#include "core/anim.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/production_json.hpp"
#include "core/smart.hpp"

namespace genko::core {

namespace pj = production;

namespace {

constexpr std::int64_t kSidePxMax = 1'000'000;

void check_footage(const Json& footage, const std::string& where) {
    pj::only_keys(footage, where, {"kind", "name", "frames", "fps", "size_px", "depth", "alpha", "par", "source"});
    const std::string kind = pj::text(pj::field(footage, "kind", where), where + ".kind", 16);
    if (kind != "sequence" && kind != "animation" && kind != "image") pj::wrong(where + ".kind", "must be sequence, animation or image");
    (void)pj::text(pj::field(footage, "name", where), where + ".name", 1024);
    const Json& frames = pj::field(footage, "frames", where);
    if (!frames.is_array() || frames.empty() || frames.size() > static_cast<std::size_t>(anim::kMaxFrames)) {
        pj::wrong(where + ".frames", "must be a list of 1.." + std::to_string(anim::kMaxFrames) + " asset refs");
    }
    for (const Json& ref : frames) {
        if (!ref.is_string() || !pj::asset_ref(ref.get_ref<const std::string&>())) pj::wrong(where + ".frames", "must hold asset refs (sha256:…)");
    }
    (void)pj::number(pj::field(footage, "fps", where), where + ".fps", std::numeric_limits<double>::min(), 1000);
    const Json& size = pj::field(footage, "size_px", where);
    if (!size.is_array() || size.size() != 2) pj::wrong(where + ".size_px", "must be [w, h]");
    (void)pj::integer(size[0], where + ".size_px", 1, kSidePxMax);
    (void)pj::integer(size[1], where + ".size_px", 1, kSidePxMax);
    const std::int64_t depth = pj::integer(pj::field(footage, "depth", where), where + ".depth", 8, 16);
    if (depth != 8 && depth != 16) pj::wrong(where + ".depth", "must be 8 or 16");
    const std::string alpha = pj::text(pj::field(footage, "alpha", where), where + ".alpha", 16);
    if (alpha != "straight" && alpha != "premultiplied" && alpha != "ignore") pj::wrong(where + ".alpha", "must be straight, premultiplied or ignore");
    (void)pj::number(pj::field(footage, "par", where), where + ".par", 0.01, 100);
    if (const Json* source = pj::optional(footage, "source")) {
        const std::string at = where + ".source";
        pj::only_keys(*source, at, {"path", "bytes", "sha256"});
        (void)pj::text(pj::field(*source, "path", at), at + ".path", 4096);
        (void)pj::integer(pj::field(*source, "bytes", at), at + ".bytes", 0, std::numeric_limits<std::int64_t>::max());
        (void)pj::text(pj::field(*source, "sha256", at), at + ".sha256", 64);
    }
}

}  // namespace

bool timeline_fps(const Json& fps) {
    if (!fps.is_number() || fps.is_boolean()) return false;
    const double v = fps.get<double>();
    if (!std::isfinite(v) || v <= 0) return false;  // (what the anims before it may hold: read as they are, fps or 12)
    return v != std::floor(v) || v > 60;
}

bool page_uses_timeline(const Page& page) {
    const Json* spec = anim::spec(page);
    if (spec == nullptr) return false;
    if (spec->contains("work")) return true;
    const auto fps = spec->find("fps");
    return fps != spec->end() && timeline_fps(*fps);
}

bool page_uses_video(const Page& page) {
    const Json* spec = anim::spec(page);
    if (spec == nullptr) return false;
    const auto tracks = spec->find("tracks");
    if (tracks == spec->end() || !tracks->is_array()) return false;
    for (const Json& track : *tracks) {
        if (track.is_object() && track.contains("video")) return true;
    }
    return false;
}

void check_timeline(const Json& anim) {
    if (!anim.is_object()) return;
    if (const auto fps = anim.find("fps"); fps != anim.end() && timeline_fps(*fps)) {
        (void)pj::number(*fps, "anim.fps", kTimelineFpsMin, kTimelineFpsMax);
    }
    if (const auto work = anim.find("work"); work != anim.end()) {
        if (!work->is_array() || work->size() != 2) pj::wrong("anim.work", "must be [first, last] (frames from 1)");
        const std::int64_t first = pj::integer((*work)[0], "anim.work", 1, anim::kMaxFrames);
        const std::int64_t last = pj::integer((*work)[1], "anim.work", 1, anim::kMaxFrames);
        if (first > last) pj::wrong("anim.work", "must have its first frame at or before its last");
    }
    if (const auto tracks = anim.find("tracks"); tracks != anim.end() && tracks->is_array()) {
        for (const Json& track : *tracks) {
            if (track.is_object() && track.contains("video")) check_video(track.at("video"));
        }
    }
}

void check_video(const Json& video) {
    const std::string where = "anim.tracks[].video";
    pj::only_keys(video, where, {"footage", "frames", "show_altered", "placement_mm", "fit"});
    if (const Json& footage = pj::field(video, "footage", where); !footage.is_null()) check_footage(footage, where + ".footage");
    const Json& frames = pj::field(video, "frames", where);
    if (!frames.is_array() || frames.size() > static_cast<std::size_t>(anim::kMaxFrames)) {
        pj::wrong(where + ".frames", "must be a list of at most " + std::to_string(anim::kMaxFrames) + " frames");
    }
    for (const Json& f : frames) {
        const bool shaped = f.is_array() && f.size() == 2 && (f[0].is_null() || (f[0].is_number_integer() && f[0].get<std::int64_t>() >= 0 &&
                                                                                 f[0].get<std::int64_t>() < anim::kMaxFrames)) &&
                            (f[1].is_null() || (f[1].is_string() && !f[1].get_ref<const std::string&>().empty()));
        if (!shaped) pj::wrong(where + ".frames", "must hold [footage frame | null, cel id | null] for each frame");
    }
    (void)pj::boolean(pj::field(video, "show_altered", where), where + ".show_altered");
    (void)pj::rect_mm(pj::field(video, "placement_mm", where), where + ".placement_mm");
    const std::string fit = pj::text(pj::field(video, "fit", where), where + ".fit", 16);
    if (fit != "contain" && fit != "cover" && fit != "stretch") pj::wrong(where + ".fit", "must be contain, cover or stretch");
}

bool uses_timeline(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (page_uses_timeline(*written.page)) return true;
    }
    return false;
}

bool uses_video(const Document& doc) {
    for (const smart::WrittenPage& written : smart::written_pages(doc)) {
        if (page_uses_video(*written.page)) return true;
    }
    return false;
}

void validate_timeline(const Document& doc) {
    const auto check = [](const Page& page) {
        if (const Json* spec = anim::spec(page); spec != nullptr && (page_uses_timeline(page) || page_uses_video(page))) check_timeline(*spec);
    };
    for (const PagePtr& page : doc.pages)
        if (page) check(*page);
    for (const smart::WrittenPage& written : smart::written_pages(doc))
        if (!written.contents.empty()) check(*written.page);
}

}  // namespace genko::core
