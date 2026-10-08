#include "core/anim.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>

#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core::anim {

namespace {

// sorted(items, key=key): the keys taken first, in the items' order (their errors first), then a stable sort by `<`.
template <typename Key>
std::vector<Json> sorted_by(const std::vector<Json>& items, Key key) {
    std::vector<std::pair<Json, std::size_t>> keyed;
    keyed.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) keyed.emplace_back(key(items[i]), i);
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return py_less(a.first, b.first); });
    std::vector<Json> out;
    out.reserve(items.size());
    for (const auto& [unused, i] : keyed) out.push_back(items[i]);
    return out;
}

// (spec(page) or {}).get(key, fallback)
Json spec_get(const Page& page, std::string_view key, const Json& fallback) {
    const Json* data = spec(page);
    return data != nullptr ? py_get(*data, key, fallback) : fallback;
}

// The values of `for t in tracks` that Python would call .get on: the items of a list (by reference); a str's
// characters and a dict's keys raise AttributeError at the first, as `t.get` would; other values are not iterable.
const Json* tracks_list(const Json& tracks) {
    if (tracks.is_array()) return &tracks;
    const std::vector<Json> items = iterate(tracks);
    if (!items.empty()) raise_attribute_error(items.front(), "get");
    return nullptr;
}

// x[key] for a key Python indexes a dict with, as a real number (frames and rects are compared and moved).
double real_at(const Json& value, std::string_view key) { return to_real(subscript(value, key)); }

}  // namespace

const Json* spec(const Page& page) {
    if (!page.extra.is_object()) return nullptr;
    const auto found = page.extra.find("anim");
    return found != page.extra.end() && found->is_object() ? &*found : nullptr;
}

bool is_animation(const Page& page) { return spec(page) != nullptr; }

std::int64_t frames_of(const Page& page) {
    return std::max<std::int64_t>(1, to_int(py_or(spec_get(page, "frames", Json()), 1)));
}

double fps_of(const Page& page) { return to_float(py_or(spec_get(page, "fps", Json()), 12)); }

bool loops(const Page& page) { return py_truthy(spec_get(page, "loop", true)); }

std::vector<Json> folders(const Page& page) {
    std::vector<Json> out;
    const Json tracks = spec_get(page, "tracks", Json::array());
    if (const Json* list = tracks_list(tracks)) {
        for (const Json& t : *list) out.push_back(py_get(t, "folder"));
    }
    return out;
}

const Json* track(const Page& page, const Json& folder_id) {
    const Json* data = spec(page);
    if (data == nullptr) return nullptr;
    const auto found = data->find("tracks");
    if (found == data->end()) return nullptr;
    const Json* list = tracks_list(*found);
    if (list == nullptr) return nullptr;
    for (const Json& t : *list) {
        if (py_equals(py_get(t, "folder"), folder_id)) return &t;
    }
    return nullptr;
}

std::vector<const Layer*> cels_of(const Page& page, const Json& folder_id) {
    std::vector<const Layer*> out;
    for (const Layer& layer : page.layers) {
        if (py_equals(layer.parent_id, folder_id)) out.push_back(&layer);
    }
    return out;
}

Json cel_at(const Page& page, const Json& folder_id, std::int64_t frame) {
    const Json* entry = track(page, folder_id);
    std::vector<Json> items;
    if (entry != nullptr && py_truthy(*entry)) items = iterate(py_get(*entry, "cels", Json::array()));
    Json shown;
    for (const Json& item : sorted_by(items, [](const Json& item) { return subscript(item, 0); })) {
        const std::vector<Json> pair = unpack_values(item, 2);
        if (py_less(pair[0], Json(frame), ">")) break;
        shown = pair[1];
    }
    return shown;
}

Page at_frame(const Page& page, std::int64_t frame) {
    const Json* data = spec(page);
    if (data == nullptr) return page;
    std::set<std::string> hide;
    const Json tracks = py_get(*data, "tracks", Json::array());
    for (const Json& entry : iterate(tracks)) {
        const Json folder = py_get(entry, "folder");
        const Json shown = cel_at(page, folder, frame);
        for (const Layer* layer : cels_of(page, folder)) {
            if (!py_equals(Json(layer->id), shown)) hide.insert(layer->id);
        }
    }
    Page out = page;
    for (Layer& layer : out.layers) {
        if (layer.visible && hide.contains(layer.id)) layer.visible = false;
    }
    return out;
}

std::optional<std::vector<double>> camera_at(const Page& page, std::int64_t frame) {
    const std::vector<Json> keys =
        sorted_by(iterate(py_or(spec_get(page, "camera", Json()), Json::array())), [](const Json& k) { return subscript(k, "frame"); });
    if (keys.empty()) return std::nullopt;
    const auto rect_of = [](const Json& key) {
        std::vector<double> out;
        for (const Json& v : iterate(subscript(key, "rect"))) out.push_back(to_real(v));
        return out;
    };
    const double at = static_cast<double>(frame);
    if (at <= real_at(keys.front(), "frame")) return rect_of(keys.front());
    for (std::size_t i = 0; i + 1 < keys.size(); ++i) {
        const double a = real_at(keys[i], "frame");
        const double b = real_at(keys[i + 1], "frame");
        if (a <= at && at <= b) {
            const double t = (at - a) / std::max(1.0, b - a);
            const std::vector<double> ra = rect_of(keys[i]);
            const std::vector<double> rb = rect_of(keys[i + 1]);
            std::vector<double> out;
            for (std::size_t k = 0; k < std::min(ra.size(), rb.size()); ++k) out.push_back(ra[k] + (rb[k] - ra[k]) * t);
            return out;
        }
    }
    return rect_of(keys.back());
}

std::vector<Json> neighbours(const Page& page, const Json& folder_id, std::int64_t frame, int direction,
                             std::int64_t how_many, bool loop) {
    const std::int64_t count = frames_of(page);
    const Json now = cel_at(page, folder_id, frame);
    std::vector<Json> out;
    std::int64_t f = frame;
    for (std::int64_t step = 0; step < count - 1; ++step) {
        f += direction;
        if (f < 1 || f > count) {
            if (!loop) break;
            f = ((f - 1) % count + count) % count + 1;  // (Python's % is never negative)
        }
        const Json cel = cel_at(page, folder_id, f);
        if (py_truthy(cel) && !py_equals(cel, now) &&
            std::none_of(out.begin(), out.end(), [&](const Json& seen) { return py_equals(seen, cel); })) {
            out.push_back(cel);
            if (static_cast<std::int64_t>(out.size()) >= how_many) break;
        }
    }
    return out;
}

}  // namespace genko::core::anim
