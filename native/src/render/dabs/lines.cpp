#include "render/dabs/lines.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <QCryptographicHash>

#include "core/base64.hpp"
#include "core/brushes.hpp"
#include "core/error.hpp"
#include "render/dabs/procedural.hpp"
#include "render/dabs/rng.hpp"
#include "render/png.hpp"
#include "storage/asset_store.hpp"

namespace genko::render::dabs {

namespace {

using core::Json;

// --- the pictures --------------------------------------------------------------------------------------------------

struct Pictures {
    std::shared_mutex mutex;
    std::map<std::string, core::Bytes> png;
    std::map<std::string, std::shared_ptr<const GrayTile>> tiles;  // (decoded once)
};

Pictures& pictures() {
    static Pictures p;
    return p;
}

// (PC4 E2) where a picture not held is looked for
std::mutex& source_mutex() {
    static std::mutex m;
    return m;
}
std::function<core::Bytes(const std::string&)>& source() {
    static std::function<core::Bytes(const std::string&)> s;
    return s;
}
core::Bytes looked_for(const std::string& ref) {
    std::function<core::Bytes(const std::string&)> find;
    {
        std::lock_guard lock(source_mutex());
        find = source();
    }
    if (!find) return nullptr;
    core::Bytes png;
    try {
        png = find(ref);
    } catch (const std::exception&) {
        return nullptr;
    }
    return keep_picture(ref, png) ? png : nullptr;
}

std::shared_ptr<const GrayTile> tile(const std::string& ref) {
    Pictures& r = pictures();
    core::Bytes png;
    {
        std::shared_lock lock(r.mutex);
        if (const auto it = r.tiles.find(ref); it != r.tiles.end()) return it->second;
        if (const auto it = r.png.find(ref); it != r.png.end()) png = it->second;
    }
    if (!png) png = looked_for(ref);
    if (!png) throw core::Error("missing_asset", "the brush picture " + ref + " is not known here (its book's brushes are not read)");
    auto made = std::make_shared<const GrayTile>(tile_of_png(*png));
    std::unique_lock lock(r.mutex);
    return r.tiles.emplace(ref, made).first->second;
}

// --- the dabs remembered -------------------------------------------------------------------------------------------

constexpr std::size_t kLinesKept = 384;
constexpr std::size_t kContextsKept = 32;

struct Remembered {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const LineDabs>> lines;
    std::deque<std::string> order;
    std::size_t dabs = 0;  // (all the lines' dabs and dual dabs)
    std::unordered_map<std::string, std::shared_ptr<const BrushContext>> contexts;
    std::deque<std::string> context_order;
};

Remembered& remembered() {
    static Remembered r;
    return r;
}

void append(std::string& key, double v) { key.append(reinterpret_cast<const char*>(&v), sizeof v); }

// What the dabs follow: the brush's settings, the line's size, seed, colours and points.
std::string line_key(const LineBrush& b, const std::vector<InputPoint>& points) {
    std::string key = core::dabs_to_json(b.settings).dump();
    append(key, b.size_mm);
    key.append(reinterpret_cast<const char*>(&b.seed), sizeof b.seed);
    for (const float c : b.color) append(key, c);
    for (const float c : b.background) append(key, c);
    for (const InputPoint& p : points) {
        for (const double v : {p.x, p.y, p.pressure, p.tilt_x, p.tilt_y, p.rotation, p.wheel, p.time}) append(key, v);
    }
    const QByteArray digest =
        QCryptographicHash::hash(QByteArrayView(key.data(), static_cast<qsizetype>(key.size())), QCryptographicHash::Sha256);
    return digest.toStdString();
}

template <typename T>
void keep_recent(std::unordered_map<std::string, T>& map, std::deque<std::string>& order, const std::string& key, T value, std::size_t most) {
    if (map.emplace(key, std::move(value)).second) order.push_back(key);
    while (order.size() > most) {
        map.erase(order.front());
        order.pop_front();
    }
}

std::shared_ptr<const PatternImage> texture_picture(const core::DabTexture& t) {
    if (!t.enabled) return nullptr;
    if (!t.pattern.asset.empty()) {
        const auto g = tile(t.pattern.asset);
        PatternImage p{g->width, g->height, g->to_f32()};
        return prepare_pattern(t, p);
    }
    return prepare_pattern(t, pattern(t.pattern.procedural, static_cast<std::uint32_t>(t.pattern.size), static_cast<std::uint32_t>(t.pattern.seed)));
}

double read_number(const Json& v, const std::string& at) {
    if (!v.is_number()) throw core::Error("value", "photocraft " + at + " must be a number");
    return v.get<double>();
}

}  // namespace

bool keep_picture(const std::string& ref, core::Bytes png) {
    if (!png || storage::AssetStore::ref(*png) != ref) return false;  // (never another picture under its name)
    Pictures& r = pictures();
    std::unique_lock lock(r.mutex);
    r.png.emplace(ref, std::move(png));
    return true;
}

void keep_pictures(const std::map<std::string, core::Bytes>& given) {
    if (given.empty()) return;
    Pictures& r = pictures();
    {
        std::shared_lock lock(r.mutex);
        if (std::all_of(given.begin(), given.end(), [&](const auto& p) { return !p.second || r.png.contains(p.first); })) return;
    }
    for (const auto& [ref, png] : given) {
        bool known = false;
        {
            std::shared_lock lock(r.mutex);
            known = r.png.contains(ref);
        }
        if (!known) keep_picture(ref, png);  // (hashed outside the lock)
    }
}

core::Bytes picture(const std::string& ref) {
    {
        Pictures& r = pictures();
        std::shared_lock lock(r.mutex);
        const auto it = r.png.find(ref);
        if (it != r.png.end()) return it->second;
    }
    return looked_for(ref);
}

void set_picture_source(std::function<core::Bytes(const std::string& ref)> find) {
    std::lock_guard lock(source_mutex());
    source() = std::move(find);
}

bool holds_picture(const std::string& ref) {
    Pictures& r = pictures();
    std::shared_lock lock(r.mutex);
    return r.png.contains(ref);
}

void clear_pictures() {
    Pictures& r = pictures();
    std::unique_lock lock(r.mutex);
    r.png.clear();
    r.tiles.clear();
    Remembered& m = remembered();
    std::lock_guard keep(m.mutex);
    m.contexts.clear();
    m.context_order.clear();
}

std::vector<InputPoint> input_points(const core::PenPoints& points, std::span<const double> rotation, const LineInputs& inputs) {
    const std::size_t n = points.size();
    const bool tilt = core::inputs_fit(std::vector<double>(inputs.tilt.begin(), inputs.tilt.end()), n, 2);
    const bool time = inputs.time_ms.size() == n && n > 0;
    const bool wheel = inputs.wheel.size() == n && n > 0;
    const bool turns = rotation.size() == n && n > 0;
    std::vector<InputPoint> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        InputPoint p;
        p.x = points[i].x;
        p.y = points[i].y;
        p.pressure = points[i].p ? std::clamp(*points[i].p, 0.0, 1.0) : 1.0;
        if (tilt) {
            p.tilt_x = inputs.tilt[2 * i];
            p.tilt_y = inputs.tilt[2 * i + 1];
        }
        if (time) p.time = inputs.time_ms[i];
        if (wheel) p.wheel = inputs.wheel[i];
        if (turns) p.rotation = rotation[i];
        out.push_back(p);
    }
    return out;
}

LineBrush line_brush(const core::DabSettings& settings, double width_mm, std::string_view line_seed, const LineInputs& inputs) {
    LineBrush b;
    b.settings = settings;
    if (inputs.flow) b.settings.flow = std::clamp(*inputs.flow, 0.0, 1.0);
    b.size_mm = width_mm;
    b.color = {inputs.color[0], inputs.color[1], inputs.color[2], 1.0f};
    if (inputs.background) b.background = {(*inputs.background)[0], (*inputs.background)[1], (*inputs.background)[2], 1.0f};
    b.seed = static_cast<std::uint64_t>(settings.seed) ^ fnv1a(line_seed);
    return b;
}

std::shared_ptr<const LineDabs> line_dabs(const LineBrush& brush, const std::vector<InputPoint>& points, bool remember) {
    if (!(brush.size_mm <= 500.0)) throw core::Error("too_large", "a dab brush's line is at most 500 mm wide");
    const std::string key = line_key(brush, points);
    Remembered& r = remembered();
    {
        std::lock_guard lock(r.mutex);
        if (const auto it = r.lines.find(key); it != r.lines.end()) return it->second;
    }
    auto made = std::make_shared<LineDabs>();
    DabGenerator g(brush);
    g.push(points, made->dabs, made->dual);
    g.finish(made->dabs, made->dual);
    if (made->dabs.size() > kMaxDabs || made->dual.size() > kMaxDabs) {
        throw core::Error("too_large", "the line has more than " + std::to_string(kMaxDabs) + " dabs (a longer line than the brush's spacing takes)");
    }
    if (dab_work(made->dabs) + dab_work(made->dual) > kMaxDabWork) {
        throw core::Error("too_large", "the line's dabs are too much to lay (their pixels past 2,000 million at 200 dpi: a wide brush at a fine spacing "
                                       "along a long line)");
    }
    std::shared_ptr<const LineDabs> out = made;
    if (!remember) return out;
    const std::size_t size = made->dabs.size() + made->dual.size();
    if (size > kDabsKept) return out;
    std::lock_guard lock(r.mutex);
    if (r.lines.emplace(key, out).second) {
        r.order.push_back(key);
        r.dabs += size;
    }
    while (!r.order.empty() && (r.order.size() > kLinesKept || r.dabs > kDabsKept)) {
        if (const auto it = r.lines.find(r.order.front()); it != r.lines.end()) {
            r.dabs -= it->second->dabs.size() + it->second->dual.size();
            r.lines.erase(it);
        }
        r.order.pop_front();
    }
    return out;
}

std::size_t remembered_dabs() {
    Remembered& r = remembered();
    std::lock_guard lock(r.mutex);
    return r.dabs;
}

std::shared_ptr<const BrushContext> context(const core::DabSettings& settings, int dpi) {
    const std::string key = core::dabs_to_json(settings).dump() + "@" + std::to_string(dpi);
    Remembered& r = remembered();
    {
        std::lock_guard lock(r.mutex);
        if (const auto it = r.contexts.find(key); it != r.contexts.end()) return it->second;
    }
    const auto mips_of = [](const std::string& ref) -> std::shared_ptr<const Mips> {
        if (ref.empty()) return nullptr;
        return std::make_shared<const Mips>(*tile(ref));
    };
    auto made = std::make_shared<const BrushContext>(settings, mips_of(settings.tip), settings.dual.enabled ? mips_of(settings.dual.tip) : nullptr,
                                                     texture_picture(settings.texture), dpi);
    std::lock_guard lock(r.mutex);
    keep_recent(r.contexts, r.context_order, key, made, kContextsKept);
    return made;
}

std::optional<Box> reach(const BrushContext& ctx, const LineDabs& dabs, Size size) {
    std::optional<Box> out;
    const auto add = [&](const Dab& d, bool dual) {
        const Box r = ctx.dab_rect(d, dual);
        const Box in{std::max(r.x0, 0), std::max(r.y0, 0), std::min(r.x1, size.width), std::min(r.y1, size.height)};
        if (in.x1 <= in.x0 || in.y1 <= in.y0) return;
        out = out ? Box{std::min(out->x0, in.x0), std::min(out->y0, in.y0), std::max(out->x1, in.x1), std::max(out->y1, in.y1)} : in;
    };
    for (const Dab& d : dabs.dabs) add(d, false);
    return out;
}

std::shared_ptr<const LineCover> cover(const core::DabSettings& settings, double width_mm, std::string_view line_seed, const core::PenPoints& points,
                                       std::span<const double> rotation, const LineInputs& inputs, int dpi, Size size, std::optional<Box> within) {
    const LineBrush brush = line_brush(settings, width_mm, line_seed, inputs);
    const auto set = line_dabs(brush, input_points(points, rotation, inputs), inputs.remember);
    Box target{0, 0, size.width, size.height};
    if (within) target = Box{std::max(target.x0, within->x0), std::max(target.y0, within->y0), std::min(target.x1, within->x1), std::min(target.y1, within->y1)};
    if (target.x1 <= target.x0 || target.y1 <= target.y0) return nullptr;
    auto laid = lay(context(settings, dpi), set->dabs, set->dual, target, settings.color.enabled && !settings.erase);
    if (laid->box.x1 <= laid->box.x0 || laid->box.y1 <= laid->box.y0) return nullptr;
    return laid;
}

// --- define_brush's pictures and upstream's settings -------------------------------------------------------------------

std::pair<std::string, core::Bytes> take_in_picture(std::string_view file, bool ink, std::uint32_t edge) {
    Image image;
    try {
        image = open_image(file, PngLimits{4096LL * 4096LL, 4096});
    } catch (const core::Error&) {
        throw core::Error("value", "the picture cannot be read (png must be a picture file in base64)");
    }
    if (image.width() <= 0 || image.height() <= 0) throw core::Error("value", "the picture holds no pixels");
    if (static_cast<std::uint32_t>(image.width()) > edge || static_cast<std::uint32_t>(image.height()) > edge) {
        throw core::Error("value", "the picture is more than " + std::to_string(edge) + " pixels a side");
    }
    std::string png;
    if (image.mode() == "L" || image.mode() == "I;16") {
        png = png_of_tile(tile_of_png(write_png(image, 9)));  // (a grey picture: its values as they are)
    } else {
        Image grey;
        const Image rgba = image.convert("RGBA");
        if (ink && rgba.getchannel(3).getextrema().front().first < 255) {
            grey = rgba.getchannel(3);  // (a tip: its alpha where it has transparency)
        } else {
            grey = rgba.convert("L");
            if (ink) grey = grey.point([](int v) { return 255 - v; });  // (else its darkness)
        }
        png = write_png(grey, 9);
    }
    auto bytes = std::make_shared<const std::string>(std::move(png));
    return {storage::AssetStore::ref(*bytes), bytes};
}

core::Json dabs_from_photocraft(const core::Json& up, std::map<std::string, core::Bytes>& held, std::vector<std::string>& ignored) {
    if (!up.is_object()) throw core::Error("value", "photocraft settings must be an object");
    const auto tile_ref = [&](const Json& t, const std::string& at) {
        if (!t.is_object() || !t.contains("width") || !t.contains("height") || !t.contains("data")) {
            throw core::Error("value", "photocraft " + at + " must be a tile {width, height, data}");
        }
        // (whole numbers 1..2500 — a tip's largest side — checked before they become ones: review P3-10)
        const auto side = [&](const char* key) {
            const double v = read_number(t[key], at + "." + key);
            if (!(v >= 1.0 && v <= 2500.0) || v != std::floor(v)) throw core::Error("value", "photocraft " + at + "." + key + " must be a whole number 1..2500");
            return static_cast<std::uint32_t>(v);
        };
        const std::uint32_t w = side("width");
        const std::uint32_t h = side("height");
        GrayTile g;
        if (t["data"].is_string()) {
            const std::string text = t["data"].get<std::string>();
            g = tile_of_json_form(w, h, &text, nullptr);
        } else if (t["data"].is_array()) {
            std::vector<double> values;
            for (const Json& v : t["data"]) values.push_back(read_number(v, at + ".data"));
            g = tile_of_json_form(w, h, nullptr, &values);
        } else {
            throw core::Error("value", "photocraft " + at + ".data must be base64 or a list of numbers");
        }
        auto png = std::make_shared<const std::string>(png_of_tile(g));
        const std::string ref = storage::AssetStore::ref(*png);
        held[ref] = png;
        return ref;
    };
    const auto tip_of = [&](const Json& t, const std::string& at) -> Json {
        if (t.is_string() && t.get<std::string>() == "round") return "round";
        if (t.is_object() && t.contains("sampled")) return Json{{"asset", tile_ref(t["sampled"], at + ".sampled")}};
        throw core::Error("value", "photocraft " + at + " must be \"round\" or {\"sampled\": a tile}");
    };
    static const std::map<std::string, std::string> kControls{{"off", "off"},           {"fade", "fade"},         {"penPressure", "pressure"},
                                                              {"penTilt", "tilt"},      {"stylusWheel", "wheel"}, {"rotation", "rotation"},
                                                              {"initialDirection", "initial_direction"}, {"direction", "direction"}};
    static const std::map<std::string, std::string> kModes{{"multiply", "multiply"},     {"subtract", "subtract"},     {"darken", "darken"},
                                                           {"overlay", "overlay"},       {"colorDodge", "color_dodge"}, {"colorBurn", "color_burn"},
                                                           {"linearBurn", "linear_burn"}, {"hardMix", "hard_mix"},     {"linearHeight", "linear_height"},
                                                           {"height", "height"}};
    const auto pick = [](const std::map<std::string, std::string>& names, const Json& v, const std::string& at) {
        if (v.is_string()) {
            if (const auto it = names.find(v.get<std::string>()); it != names.end()) return it->second;
        }
        throw core::Error("value", "photocraft " + at + " is not one upstream names");
    };
    const auto dynamic = [&](const Json& d, const std::string& at) {
        Json out = Json::object();
        if (!d.is_object()) throw core::Error("value", "photocraft " + at + " must be an object");
        for (const auto& [k, v] : d.items()) {
            if (k == "jitter") out["jitter"] = read_number(v, at + ".jitter");
            else if (k == "control") out["control"] = pick(kControls, v, at + ".control");
            else if (k == "fadeSteps" || k == "fade_steps") out["fade_steps"] = v;
            else if (k == "minimum") out["minimum"] = read_number(v, at + ".minimum");
            else ignored.push_back(at + "." + k);
        }
        return out;
    };
    // a section: its keys mapped (camelCase → snake_case), the dynamics and the pixels as mm
    struct Key {
        const char* up;
        const char* mine;
        enum Kind { Plain, Px, Dynamic, Mode, Tip } kind;
    };
    const auto section = [&](const Json& obj, std::initializer_list<Key> keys, const std::string& at) {
        if (!obj.is_object()) throw core::Error("value", "photocraft " + at + " must be an object");
        Json out = Json::object();
        for (const auto& [k, v] : obj.items()) {
            const Key* found = nullptr;
            for (const Key& key : keys) {
                if (k == key.up) found = &key;
            }
            if (found == nullptr) {
                ignored.push_back(at.empty() ? k : at + "." + k);
                continue;
            }
            const std::string here = at.empty() ? k : at + "." + k;
            switch (found->kind) {
                case Key::Plain: out[found->mine] = v; break;
                case Key::Px: out[found->mine] = read_number(v, here) * kPx; break;
                case Key::Dynamic: out[found->mine] = dynamic(v, here); break;
                case Key::Mode: out[found->mine] = pick(kModes, v, here); break;
                case Key::Tip: out[found->mine] = tip_of(v, here); break;
            }
        }
        return out;
    };
    Json out = section(up,
                       {{"size", "size_mm", Key::Px},
                        {"hardness", "hardness", Key::Plain},
                        {"spacing", "spacing", Key::Plain},
                        {"opacity", "opacity", Key::Plain},
                        {"flow", "flow", Key::Plain},
                        {"pressureSize", "pressure_size", Key::Plain},
                        {"pressureOpacity", "pressure_opacity", Key::Plain},
                        {"pressure_size", "pressure_size", Key::Plain},  // (upstream's aliases)
                        {"pressure_opacity", "pressure_opacity", Key::Plain},
                        {"erase", "erase", Key::Plain},
                        {"tip", "tip", Key::Tip},
                        {"angle", "angle", Key::Plain},
                        {"roundness", "roundness", Key::Plain},
                        {"flipX", "flip_x", Key::Plain},
                        {"flipY", "flip_y", Key::Plain},
                        {"aliased", "aliased", Key::Plain},
                        {"noise", "noise", Key::Plain},
                        {"wetEdges", "wet_edges", Key::Plain},
                        {"buildUp", "build_up", Key::Plain},
                        {"buildUpRate", "build_up_rate", Key::Plain},
                        {"protectTexture", "protect_texture", Key::Plain},
                        {"seed", "seed", Key::Plain},
                        {"shapeDynamics", "shape", Key::Plain},
                        {"scattering", "scatter", Key::Plain},
                        {"texture", "texture", Key::Plain},
                        {"dualBrush", "dual", Key::Plain},
                        {"colorDynamics", "color", Key::Plain},
                        {"transfer", "transfer", Key::Plain},
                        {"pose", "pose", Key::Plain}},
                       "");
    // (the line's colours and blend and the window's smoothing — color, background, mode, smoothing — are not the
    // brush's here: they are among `ignored`)
    if (out.contains("shape")) {
        out["shape"] = section(out["shape"],
                               {{"enabled", "enabled", Key::Plain},
                                {"size", "size", Key::Dynamic},
                                {"angle", "angle", Key::Dynamic},
                                {"roundness", "roundness", Key::Dynamic},
                                {"flipXJitter", "flip_x_jitter", Key::Plain},
                                {"flipYJitter", "flip_y_jitter", Key::Plain}},
                               "shapeDynamics");
    }
    if (out.contains("scatter")) {
        out["scatter"] = section(out["scatter"],
                                 {{"enabled", "enabled", Key::Plain},
                                  {"scatter", "scatter", Key::Dynamic},
                                  {"bothAxes", "both_axes", Key::Plain},
                                  {"count", "count", Key::Plain},
                                  {"countJitter", "count_jitter", Key::Dynamic}},
                                 "scattering");
    }
    if (out.contains("texture")) {
        Json given = out["texture"];
        Json pattern;
        if (given.is_object() && given.contains("pattern")) {
            const Json p = given["pattern"];
            given.erase("pattern");
            if (p.is_object() && p.contains("procedural") && p["procedural"].is_object()) {
                const Json& pr = p["procedural"];
                pattern = Json::object();
                if (pr.contains("style")) pattern["procedural"] = pr["style"];
                if (pr.contains("size")) pattern["size"] = pr["size"];
                if (pr.contains("seed")) pattern["seed"] = pr["seed"];
            } else if (p.is_object() && p.contains("tile")) {
                pattern = Json{{"asset", tile_ref(p["tile"], "texture.pattern.tile")}};
            } else {
                throw core::Error("value", "photocraft texture.pattern must be {\"procedural\": …} or {\"tile\": a tile}");
            }
        }
        Json t = section(given,
                         {{"enabled", "enabled", Key::Plain},
                          {"invert", "invert", Key::Plain},
                          {"scale", "scale", Key::Plain},
                          {"brightness", "brightness", Key::Plain},
                          {"contrast", "contrast", Key::Plain},
                          {"eachTip", "each_tip", Key::Plain},
                          {"mode", "mode", Key::Mode},
                          {"depth", "depth", Key::Plain},
                          {"depthJitter", "depth_jitter", Key::Dynamic}},
                         "texture");
        if (!pattern.is_null()) t["pattern"] = pattern;
        out["texture"] = t;
    }
    if (out.contains("dual")) {
        out["dual"] = section(out["dual"],
                              {{"enabled", "enabled", Key::Plain},
                               {"mode", "mode", Key::Mode},
                               {"tip", "tip", Key::Tip},
                               {"size", "size_mm", Key::Px},
                               {"hardness", "hardness", Key::Plain},
                               {"roundness", "roundness", Key::Plain},
                               {"angle", "angle", Key::Plain},
                               {"spacing", "spacing", Key::Plain},
                               {"scatter", "scatter", Key::Plain},
                               {"bothAxes", "both_axes", Key::Plain},
                               {"count", "count", Key::Plain},
                               {"flip", "flip", Key::Plain}},
                              "dualBrush");
    }
    if (out.contains("color")) {
        out["color"] = section(out["color"],
                               {{"enabled", "enabled", Key::Plain},
                                {"perTip", "per_tip", Key::Plain},
                                {"fgBg", "fg_bg", Key::Dynamic},
                                {"hueJitter", "hue_jitter", Key::Plain},
                                {"saturationJitter", "saturation_jitter", Key::Plain},
                                {"brightnessJitter", "brightness_jitter", Key::Plain},
                                {"purity", "purity", Key::Plain}},
                               "colorDynamics");
    }
    if (out.contains("transfer")) {
        out["transfer"] = section(out["transfer"],
                                  {{"enabled", "enabled", Key::Plain}, {"opacity", "opacity", Key::Dynamic}, {"flow", "flow", Key::Dynamic}},
                                  "transfer");
    }
    if (out.contains("pose")) {
        out["pose"] = section(out["pose"],
                              {{"enabled", "enabled", Key::Plain},
                               {"tiltX", "tilt_x", Key::Plain},
                               {"tiltY", "tilt_y", Key::Plain},
                               {"rotation", "rotation", Key::Plain},
                               {"pressure", "pressure", Key::Plain},
                               {"overrideTilt", "override_tilt", Key::Plain},
                               {"overrideRotation", "override_rotation", Key::Plain},
                               {"overridePressure", "override_pressure", Key::Plain}},
                              "pose");
    }
    // (checked, and only what differs from the defaults kept)
    return core::dabs_to_json(core::dabs_from_json(out));
}

core::Json pencil_brush() {
    return Json{{"label", "鉛筆（ハード）"},
                {"engine", std::string(core::kDabEngine)},
                {"dabs", Json{{"hardness", 1.0}, {"aliased", true}, {"pressure_size", false}}}};
}

bool is_pencil(const core::Json& settings) {
    static const Json pencil = core::brush_to_dict(core::brush_from_dict(kPencilKey, pencil_brush(), std::nullopt, {}));
    return settings == pencil;
}

}  // namespace genko::render::dabs
