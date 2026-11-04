// PC5 J-e2: a .pcraft's timeline as the page's animation, and back (render/pcraft_timeline.hpp).

#include "render/pcraft_timeline.hpp"

#include <algorithm>
#include <cmath>

#include "core/anim.hpp"
#include "core/video.hpp"

namespace genko::render::pcraft_timeline {

using core::Json;

namespace {

// A whole number of 0.. (serde's usize), none when it is not one.
std::optional<std::int64_t> count_of(const Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end()) return std::nullopt;
    if (found->is_number_unsigned()) return found->get<std::uint64_t>() > static_cast<std::uint64_t>(INT64_MAX) ? std::nullopt : std::optional<std::int64_t>(static_cast<std::int64_t>(found->get<std::uint64_t>()));
    if (found->is_number_integer() && found->get<std::int64_t>() >= 0) return found->get<std::int64_t>();
    return std::nullopt;
}

}  // namespace

void read(const Json& value, pcraft_production::ReadContext& context) {
    if (value.is_null() || context.page == nullptr) return;
    const auto unreadable = [&] {
        if (context.say) context.say("タイムラインは読めない形のため外しました");
    };
    if (!value.is_object()) return unreadable();
    const auto fps_at = value.find("fps");
    const auto duration = count_of(value, "duration");
    const auto end = count_of(value, "work_end");
    const auto start = value.contains("work_start") ? count_of(value, "work_start") : std::optional<std::int64_t>(0);
    if (fps_at == value.end() || !fps_at->is_number() || fps_at->is_boolean() || !duration || !end || !start || !count_of(value, "current")) {
        return unreadable();
    }
    double fps = fps_at->get<double>();
    if (!std::isfinite(fps)) return unreadable();
    const std::int64_t frames = std::clamp<std::int64_t>(*duration, 1, core::anim::kMaxFrames);
    if (*duration > core::anim::kMaxFrames && context.say) context.say("タイムラインは 3000 フレームまでにしました");
    if (fps < core::kTimelineFpsMin || fps > core::kTimelineFpsMax) {
        fps = std::clamp(fps > 0 ? fps : 30.0, core::kTimelineFpsMin, core::kTimelineFpsMax);  // (upstream's: 30 for none)
        if (context.say) context.say("タイムラインのフレームレートは 0.5〜120 にしました");
    }
    fps = std::round(fps * 1000) / 1000;  // (an f32's digits: 3 places, as the timeline's field shows it)
    Json anim = Json::object();
    anim["fps"] = std::nearbyint(fps) == fps && fps >= 1 && fps <= 60 ? Json(static_cast<std::int64_t>(fps)) : Json(fps);
    anim["frames"] = frames;
    anim["loop"] = true;
    anim["tracks"] = Json::array();
    // (upstream's clamp: the work area inside the frames, at least one)
    const std::int64_t first = std::min(*start, frames - 1) + 1;
    const std::int64_t last = std::clamp(*end, first, frames);
    if (first > 1 || last < frames) anim["work"] = Json::array({first, last});
    if (!context.page->extra.is_object()) context.page->extra = Json::object();
    context.page->extra["anim"] = std::move(anim);
}

std::optional<Json> write(const core::Page& page, pcraft_production::WriteContext&) {
    if (!core::anim::is_animation(page)) return std::nullopt;
    std::int64_t frames = 1;
    double fps = 12;
    std::array<std::int64_t, 2> played{1, 1};
    try {
        frames = core::anim::frames_of(page);
        fps = core::anim::fps_of(page);
        played = core::played_frames(page);
    } catch (const std::exception&) {  // (an anim's values no op makes: its speed and length as the timeline shows them)
        played = {1, frames};
    }
    if (!(fps > 0) || !std::isfinite(fps)) fps = 12;
    Json out = Json::object();
    out["fps"] = fps;
    out["duration"] = frames;
    out["current"] = 0;
    out["work_start"] = played[0] - 1;
    out["work_end"] = played[1];
    return out;
}

}  // namespace genko::render::pcraft_timeline
