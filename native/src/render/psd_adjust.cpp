// The codecs of a PSD's adjustment layers (render/psd_adjust.hpp): the table of every adjustment block and the two
// ways through it. PC2a writes the tone kinds' codecs here (tone_codecs: levl, curv, brit with its CgEd, post, thrs,
// nvrt, expA, grdm, clrL — PhotoCraft v0.2.0 io/adjust_map.rs 21-110, 160-316, 331-375, 487-640 and
// io/gradient_bake.rs; attribution in native/third_party/photocraft/NOTICE.md). The layouts are Adobe's ("Adjustment
// layer"), as psd-tools reads them.

#include "render/psd_adjust.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

#include "core/adjust.hpp"
#include "core/adjust_kinds.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/exposure.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/psd_descriptor.hpp"

namespace genko::render::psd {

namespace {

using core::Json;

AdjustCodec codec(std::string_view key, std::string_view kind, std::string_view group) {
    AdjustCodec out;
    out.key = key;
    out.kind = kind;
    out.group = group;
    return out;
}

std::vector<AdjustCodec> make_table() {
    // (hue2 twice: hue_saturation's, read and written, then Python's hue written as the nearest — read as the first)
    std::vector<AdjustCodec> all{codec("levl", "levels", "PC2a"),         codec("curv", "curve", "PC2a"),
                                 codec("brit", "brightness_contrast", "PC2a"), codec("post", "posterize", "PC2a"),
                                 codec("thrs", "threshold", "PC2a"),       codec("nvrt", "invert", "PC2a"),
                                 codec("expA", "exposure", "PC2a"),        codec("grdm", "gradient_map", "PC2a"),
                                 codec("clrL", "color_lookup", "PC2a"),    codec("hue2", "hue_saturation", "PC2b"),
                                 codec("hue2", "hue", "PC2b"),             codec("vibA", "vibrance", "PC2b"),
                                 codec("blnc", "color_balance", "PC2b"),   codec("blwh", "black_white", "PC2b"),
                                 codec("phfl", "photo_filter", "PC2b"),    codec("mixr", "channel_mixer", "PC2b"),
                                 codec("selc", "selective_color", "PC2b")};
    for (const CodecHooks& hooks : {tone_codecs(), color_codecs()}) {
        for (const auto& [key, given] : hooks) {
            for (AdjustCodec& each : all) {
                if (each.key != key || (!given.kind.empty() && given.kind != each.kind)) continue;
                if (given.read) each.read = given.read;
                if (given.write) each.write = given.write;
            }
        }
    }
    return all;
}

thread_local AdjustContext current;

// (writing) a companion block a codec made: brightness_contrast's CgEd
std::string& pending_companion() {
    thread_local std::string companion;
    return companion;
}

// --- bytes ----------------------------------------------------------------------------------------------------------

std::optional<std::uint32_t> be16(std::string_view d, std::size_t at) {
    if (at + 2 > d.size()) return std::nullopt;
    return (std::uint32_t(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]);
}
std::optional<std::int32_t> bei16(std::string_view d, std::size_t at) {
    const auto v = be16(d, at);
    if (!v) return std::nullopt;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(*v));
}
std::optional<std::uint32_t> be32(std::string_view d, std::size_t at) {
    const auto hi = be16(d, at), lo = be16(d, at + 2);
    if (!hi || !lo) return std::nullopt;
    return (*hi << 16) | *lo;
}
std::optional<double> bef32(std::string_view d, std::size_t at) {
    const auto bits = be32(d, at);
    if (!bits) return std::nullopt;
    float v = 0;
    std::memcpy(&v, &*bits, sizeof v);
    return v;
}

void put16(std::string& out, std::uint32_t v) {
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>(v & 0xff);
}
void put_i16(std::string& out, double v) { put16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(std::round(v), -32768.0, 32767.0)))); }
void put32(std::string& out, std::uint32_t v) {
    put16(out, v >> 16);
    put16(out, v & 0xffff);
}
void put_f32(std::string& out, double v) {
    const float f = static_cast<float>(v);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    put32(out, bits);
}
std::uint32_t q255(double v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))); }

// --- descriptors (what CgEd and clrL hold: render/psd_descriptor, the one reader and writer) ---------------------------

// A versioned descriptor (version 16) at `at`; none when it cannot be read.
std::optional<descriptor::Object> versioned(std::string_view d, std::size_t at = 0) {
    try {
        return descriptor::read_versioned(d, at);
    } catch (const descriptor::Broken&) {
        return std::nullopt;
    }
}

// A descriptor's number (long, doub, UntF, comp), truth value, or text (TEXT: UTF-8; enum: its value; tdta: the bytes).
std::optional<double> number_of(const descriptor::Object& v, const char* key) {
    const descriptor::Value* it = v.get(key);
    if (it == nullptr || !(it->type == "long" || it->type == "doub" || it->type == "UntF" || it->type == "comp")) return std::nullopt;
    return it->numeric();
}
std::optional<bool> bool_of(const descriptor::Object& v, const char* key) {
    const descriptor::Value* it = v.get(key);
    if (it == nullptr || it->type != "bool") return std::nullopt;
    return it->boolean;
}
std::optional<std::string> text_of(const descriptor::Object& v, const char* key, std::string_view type) {
    const descriptor::Value* it = v.get(key);
    if (it == nullptr || it->type != type) return std::nullopt;
    if (type == "TEXT") return utf8_of(it->text);
    if (type == "enum") return it->enum_value;
    return it->raw;
}

// --- a correction's settings and its page ----------------------------------------------------------------------------

Json number_json(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

// The space of a PSD's channels: the page's mode (a page of inks its space; an RGB page rgb).
std::string_view page_space(const core::Page& page) {
    if (!page.color) return "rgb";
    switch (page.color->space) {
        case core::InkSpace::Cmyk: return "cmyk";
        case core::InkSpace::Lab: return "lab";
        case core::InkSpace::Gray: return "gray";
        default: return "other";
    }
}

std::string native_text(const Json& adjust, const char* key) {
    const auto native = adjust.find("native");
    if (native == adjust.end() || !native->is_object()) return {};
    const auto it = native->find(key);
    return it != native->end() && it->is_string() ? it->get<std::string>() : std::string();
}
bool native_flag(const Json& adjust, const char* key) {
    const auto native = adjust.find("native");
    return native != adjust.end() && native->is_object() && native->value(key, Json()) == Json(true);
}
// float(x) of a Python setting, or the fallback (core::PyValueError, PyTypeError for one Python cannot read).
double setting(const Json& adjust, const char* key, double fallback) {
    const Json* v = core::get(adjust, key);
    return v == nullptr || v->is_null() ? fallback : core::to_float(*v);
}
double whole_setting(const Json& adjust, const char* key, double fallback) {
    const Json* v = core::get(adjust, key);
    if (v == nullptr || v->is_null()) return fallback;
    return v->is_number_float() ? core::py_trunc(v->get<double>()) : static_cast<double>(core::to_int(*v));
}
int python_channel(const Json& adjust) {
    const Json* given = core::get(adjust, "channel");
    const std::string name = given != nullptr && core::py_truthy(*given) ? core::py_str(*given) : std::string("rgb");
    return name == "r" ? 0 : name == "g" ? 1 : name == "b" ? 2 : -1;
}
const Json* records_of(const Json& adjust) {
    const auto native = adjust.find("native");
    if (native == adjust.end() || !native->is_object()) return nullptr;
    const auto it = native->find("channels");
    return it != native->end() && it->is_object() && !it->empty() ? &*it : nullptr;
}

// --- levl -------------------------------------------------------------------------------------------------------------

struct LevelsRecord {
    double black = 0, white = 255, gamma = 1, out_black = 0, out_white = 255;
    bool identity() const { return black == 0 && white == 255 && gamma == 1 && out_black == 0 && out_white == 255; }
    Json json() const {
        return Json{{"black", number_json(black)}, {"white", number_json(white)}, {"gamma", number_json(gamma)},
                    {"out_black", number_json(out_black)}, {"out_white", number_json(out_white)}};
    }
};

std::optional<LevelsRecord> levels_at(std::string_view d, std::size_t at) {
    const auto a = be16(d, at), b = be16(d, at + 2), c = be16(d, at + 4), e = be16(d, at + 6), g = be16(d, at + 8);
    if (!a || !b || !c || !e || !g) return std::nullopt;
    LevelsRecord r;
    r.black = *a;
    r.white = *b;
    r.out_black = *c;
    r.out_white = *e;
    r.gamma = *g / 100.0;
    return r;
}

void put_levels(std::string& out, const LevelsRecord& r) {
    const double black = std::clamp(std::round(r.black), 0.0, 253.0);
    put16(out, q255(black));
    put16(out, q255(std::max(std::clamp(std::round(r.white), 2.0, 255.0), black + 2)));
    put16(out, q255(r.out_black));
    put16(out, q255(r.out_white));
    put16(out, static_cast<std::uint32_t>(std::clamp(std::round(r.gamma * 100.0), 1.0, 999.0)));
}

// The PSD's channel records as a correction's native (only the ones that change something): its space by the PSD's
// mode, read as an op reads them.
Json native_records(std::string_view kind, const Json& raw) {
    Json native{{"method", "upstream"}};
    if (raw.empty()) return native;
    const std::string space = std::string(current.mode);
    native["space"] = space;
    native["channels"] = core::read_tone_channels(kind, raw, space, core::ReadMode::Op, std::string(kind));
    return native;
}

std::optional<Json> read_levels(std::string_view d, std::vector<std::string>& notes) {
    const auto master = levels_at(d, 2);
    if (!master || !levels_at(d, 32)) {
        note_unread(notes, "levl", "途中で切れている");
        return std::nullopt;
    }
    const std::string_view mode = current.mode;
    const std::vector<std::string_view> names = mode == "other" ? std::vector<std::string_view>{} : core::tone_channels(mode);
    Json raw = Json::object();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto r = levels_at(d, 2 + 10 * (i + 1));
        if (r && !r->identity()) raw[std::string(names[i])] = r->json();
    }
    // (a Lab document's levels have no composite: PhotoCraft leaves it out)
    const LevelsRecord composite = mode == "lab" ? LevelsRecord{} : *master;
    Json out{{"kind", "levels"}};
    const Json record = composite.json();
    for (const auto& [key, value] : record.items()) out[key] = value;
    out["native"] = native_records("levels", raw);
    return out;
}

std::optional<std::string> write_levels(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const std::string_view space = page_space(page);
    if (space == "other") return std::nullopt;
    LevelsRecord composite;
    try {
        static constexpr std::array<const char*, 4> kTable{"gamma", "out_black", "out_white", "channel"};
        const bool table = std::any_of(kTable.begin(), kTable.end(), [&adjust](const char* key) {
            const Json* v = core::get(adjust, key);
            return v != nullptr && !v->is_null();
        });
        composite.black = table ? setting(adjust, "black", 0) : whole_setting(adjust, "black", 0);
        composite.white = table ? setting(adjust, "white", 255) : whole_setting(adjust, "white", 255);
        composite.gamma = std::clamp(setting(adjust, "gamma", 1.0), 0.1, 9.99);
        composite.out_black = setting(adjust, "out_black", 0);
        composite.out_white = setting(adjust, "out_white", 255);
    } catch (const core::Error&) {
        note_unwritten(notes, "levels", "値が読めない");
        return std::nullopt;
    }
    const std::vector<std::string_view> names = core::tone_channels(space);
    std::vector<LevelsRecord> slots(names.size());
    std::vector<bool> given(names.size(), false);
    const Json* records = records_of(adjust);
    if (records != nullptr) {
        if (native_text(adjust, "space") != space) {
            notes.emplace_back("レベル補正のチャンネルごとの組は、ページと違う色空間のため PSD に書いていません（合成の組だけ）");
        } else {
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto it = records->find(names[i]);
                if (it == records->end()) continue;
                given[i] = true;
                slots[i].black = it->value("black", 0.0);
                slots[i].white = it->value("white", 255.0);
                slots[i].gamma = it->value("gamma", 1.0);
                slots[i].out_black = it->value("out_black", 0.0);
                slots[i].out_white = it->value("out_white", 255.0);
            }
        }
    }
    // (Python's "channel": the composite on one colour — its record; Lab: the composite on the lightness)
    const int only = space == "rgb" ? python_channel(adjust) : space == "lab" ? 0 : -1;
    if (only >= 0 && !composite.identity()) {
        if (given[static_cast<std::size_t>(only)]) {
            notes.emplace_back("レベル補正の合成の組を 1 つの色だけにかける形は PSD で表せないため、全体にかけて書きました");
        } else {
            slots[static_cast<std::size_t>(only)] = composite;
            composite = LevelsRecord{};
        }
    }
    if (native_text(adjust, "method") != "upstream") {
        const bool soft = composite.gamma > 1 || std::any_of(slots.begin(), slots.end(), [](const LevelsRecord& r) { return r.gamma > 1; });
        if (soft) notes.emplace_back("Genko の式（genko）のレベル補正は、Photoshop で開くと中間調のガンマ（1 より大）の暗い所が少し違うことがあります");
    }
    std::string out;
    put16(out, 2);
    put_levels(out, composite);
    for (std::size_t i = 0; i < 28; ++i) put_levels(out, i < slots.size() ? slots[i] : LevelsRecord{});
    return out;
}

// --- curv -------------------------------------------------------------------------------------------------------------

using Points = std::vector<std::array<double, 2>>;

Json points_json(const Points& points) {
    Json out = Json::array();
    for (const auto& p : points) out.push_back(Json::array({number_json(p[0]), number_json(p[1])}));
    return out;
}

bool straight(const Points& p) { return p.size() == 2 && p[0][0] == 0 && p[0][1] == 0 && p[1][0] == 255 && p[1][1] == 255; }

std::optional<Json> read_curves(std::string_view d, std::vector<std::string>& notes) {
    const auto version = be16(d, 1);
    const auto bits = be32(d, 3);
    if (!version || (*version != 1 && *version != 4) || !bits) {
        note_unread(notes, "curv", "読めない版の");
        return std::nullopt;
    }
    std::size_t at = 7;
    std::vector<Points> curves(5, Points{{0, 0}, {255, 255}});
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if ((*bits & (1u << bit)) == 0) continue;
        const auto n = be16(d, at);
        if (!n || *n > 64) {
            note_unread(notes, "curv", "途中で切れている");
            return std::nullopt;
        }
        at += 2;
        Points points;
        for (std::uint32_t i = 0; i < *n; ++i) {
            const auto out = be16(d, at), in = be16(d, at + 2);
            if (!out || !in) {
                note_unread(notes, "curv", "途中で切れている");
                return std::nullopt;
            }
            points.push_back({static_cast<double>(*in), static_cast<double>(*out)});
            at += 4;
        }
        if (bit < curves.size()) curves[bit] = std::move(points);
    }
    const std::string_view mode = current.mode;
    const std::vector<std::string_view> names = mode == "other" ? std::vector<std::string_view>{} : core::tone_channels(mode);
    Json raw = Json::object();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Points& c = curves[i + 1];
        if (c.size() >= 2 && !straight(c)) raw[std::string(names[i])] = points_json(c);
    }
    Points composite = mode == "lab" || curves[0].size() < 2 ? Points{{0, 0}, {255, 255}} : curves[0];
    std::sort(composite.begin(), composite.end());
    Json out{{"kind", "curve"}, {"points", points_json(composite)}};
    try {
        out["native"] = native_records("curve", raw);
    } catch (const core::OpError&) {
        note_unread(notes, "curv", "チャンネルごとの曲線が使えない");
        return std::nullopt;
    }
    return out;
}

// A curve's points as Python reads them (x rounded, a later one at the same x wins, sorted), at most 19 (more taken at
// even steps), each in 0..255.
std::optional<Points> python_points(const Json& adjust) {
    const Json* given = core::get(adjust, "points");
    if (given == nullptr || !core::py_truthy(*given)) return std::nullopt;
    Points pts;
    for (const Json& item : core::iterate(*given)) {
        const std::vector<Json> xy = core::iterate(item);
        if (xy.size() != 2) return std::nullopt;
        const double x = core::py_round_whole(core::to_float(xy[0])), y = std::clamp(core::to_float(xy[1]), 0.0, 255.0);
        const auto at = std::find_if(pts.begin(), pts.end(), [x](const auto& p) { return p[0] == x; });
        if (at != pts.end()) (*at)[1] = y;
        else pts.push_back({x, y});
    }
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a[0] < b[0]; });
    for (auto& p : pts) p[0] = std::clamp(p[0], 0.0, 255.0);
    return pts;
}

Points thinned(const Points& points, bool& thinned_out) {
    if (points.size() <= 19) return points;
    thinned_out = true;
    Points out;
    for (std::size_t k = 0; k < 19; ++k) out.push_back(points[k * (points.size() - 1) / 18]);
    return out;
}

void put_curve(std::string& out, const Points& points) {
    put16(out, static_cast<std::uint32_t>(points.size()));
    for (const auto& p : points) {
        put16(out, q255(p[1]));
        put16(out, q255(p[0]));
    }
}

std::optional<std::string> write_curves(const Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const std::string_view space = page_space(page);
    if (space == "other") return std::nullopt;
    Points composite{{0, 0}, {255, 255}};
    bool approximated = false;
    try {
        if (const auto pts = python_points(adjust)) {
            if (pts->size() < 2) return std::nullopt;
            composite = thinned(*pts, approximated);
        } else {  // (Python's gamma curve, at 17 points)
            const double gamma = std::clamp(setting(adjust, "gamma", 1.6), 0.2, 5.0);
            composite.clear();
            for (int k = 0; k <= 16; ++k) {
                const double x = std::min(255.0, k * 16.0);
                composite.push_back({x, std::round(255 * std::pow(x / 255, gamma))});
            }
            approximated = true;
        }
    } catch (const core::Error&) {
        note_unwritten(notes, "curve", "点が読めない");
        return std::nullopt;
    }
    const std::vector<std::string_view> names = core::tone_channels(space);
    std::vector<Points> slots(names.size(), Points{{0, 0}, {255, 255}});
    std::vector<bool> given(names.size(), false);
    if (const Json* records = records_of(adjust)) {
        if (native_text(adjust, "space") != space) {
            notes.emplace_back("トーンカーブのチャンネルごとの曲線は、ページと違う色空間のため PSD に書いていません（合成の曲線だけ）");
        } else {
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto it = records->find(names[i]);
                if (it == records->end()) continue;
                given[i] = true;
                slots[i].clear();
                for (const Json& p : *it) slots[i].push_back({p[0].get<double>(), p[1].get<double>()});
            }
        }
    }
    const int only = space == "rgb" ? python_channel(adjust) : space == "lab" ? 0 : -1;
    if (only >= 0 && !straight(composite)) {
        if (given[static_cast<std::size_t>(only)]) {
            notes.emplace_back("トーンカーブの合成の曲線を 1 つの色だけにかける形は PSD で表せないため、全体にかけて書きました");
        } else {
            slots[static_cast<std::size_t>(only)] = composite;
            composite = Points{{0, 0}, {255, 255}};
        }
    }
    if (approximated) notes.emplace_back("トーンカーブの曲線を Photoshop の点（19 点まで）に直して書きました（少し違うことがあります）");
    if (native_text(adjust, "method") != "upstream") {
        const bool bends = composite.size() > 2 || std::any_of(slots.begin(), slots.end(), [](const Points& p) { return p.size() > 2; });
        if (bends) notes.emplace_back("Genko の式（genko）のトーンカーブは、Photoshop で開くと点の間の曲がり方が少し違うことがあります");
    }
    std::string out;
    out += '\0';
    put16(out, 1);
    const std::uint32_t bits = (1u << (names.size() + 1)) - 1;  // (the composite and each channel of the page's mode)
    put32(out, bits);
    put_curve(out, composite);
    for (const Points& p : slots) put_curve(out, p);
    // Photoshop's "Crv " part after them (version 4: each curve again with its channel's index), as Photoshop writes it
    // and psd-tools reads it (PhotoCraft writes the first part only)
    out += "Crv ";
    put16(out, 4);
    put32(out, static_cast<std::uint32_t>(slots.size() + 1));
    for (std::size_t i = 0; i <= slots.size(); ++i) {
        put16(out, static_cast<std::uint32_t>(i));
        put_curve(out, i == 0 ? composite : slots[i - 1]);
    }
    return out;
}

// --- brit (and its CgEd), post, thrs, nvrt, expA --------------------------------------------------------------------

std::optional<Json> read_brightness(std::string_view d, std::vector<std::string>& notes) {
    if (!current.companion.empty()) {
        if (const auto values = versioned(current.companion)) {
            const auto b = number_of(*values, "Brgh"), c = number_of(*values, "Cntr");
            if (b && c) {
                const bool legacy = bool_of(*values, "useLegacy").value_or(false);
                return Json{{"kind", "brightness_contrast"}, {"brightness", number_json(std::clamp(*b, -150.0, 150.0))},
                            {"contrast", number_json(std::clamp(*c, legacy ? -100.0 : -50.0, 100.0))},
                            {"native", Json{{"method", legacy ? "legacy" : "modern"}}}};
            }
        }
    }
    const auto b = bei16(d, 0), c = bei16(d, 2);
    if (!b || !c) {
        note_unread(notes, "brit", "途中で切れている");
        return std::nullopt;
    }
    return Json{{"kind", "brightness_contrast"}, {"brightness", std::clamp(*b, -150, 150)}, {"contrast", std::clamp(*c, -100, 100)},
                {"native", Json{{"method", "legacy"}}}};
}

std::optional<std::string> write_brightness(const Json& adjust, const core::Page&, std::vector<std::string>& notes) {
    std::string method = native_text(adjust, "method");
    double brightness = 0, contrast = 0;
    try {
        brightness = setting(adjust, "brightness", 0);
        contrast = setting(adjust, "contrast", 0);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    if (!std::isfinite(brightness) || !std::isfinite(contrast)) return std::nullopt;
    if (method.empty()) {  // (Python's linear form: Photoshop's legacy one is the nearest — brightness in levels)
        brightness = std::clamp(brightness, -100.0, 100.0) * 1.275;
        contrast = std::clamp(contrast, -100.0, 100.0);
        method = "legacy";
        notes.emplace_back("Genko（Python）の明るさ・コントラストは、Photoshop の従来の式（legacy）として書きました（少し違うことがあります）");
    }
    if (python_channel(adjust) >= 0) notes.emplace_back("明るさ・コントラストを 1 つの色だけにかける形は PSD で表せないため、全体にかけて書きました");
    const bool legacy = method == "legacy";
    brightness = std::round(std::clamp(brightness, -150.0, 150.0));
    contrast = std::round(std::clamp(contrast, legacy ? -100.0 : -50.0, 100.0));
    std::string out;
    put_i16(out, brightness);
    put_i16(out, contrast);
    put16(out, 127);  // (mean)
    out += '\0';      // (Lab only)
    descriptor::Writer d;
    d.integer("Vrsn", 1);
    d.integer("Brgh", static_cast<std::int32_t>(brightness));
    d.integer("Cntr", static_cast<std::int32_t>(contrast));
    d.integer("means", 127);
    d.boolean("Lab ", false);
    d.boolean("useLegacy", legacy);
    d.boolean("Auto", false);
    pending_companion() = d.versioned("null");  // (the CgEd block: write_adjustment takes it)
    return out;
}

std::optional<Json> read_posterize(std::string_view d, std::vector<std::string>& notes) {
    const auto n = be16(d, 0);
    if (!n) {
        note_unread(notes, "post", "途中で切れている");
        return std::nullopt;
    }
    return Json{{"kind", "posterize"}, {"levels", std::clamp<std::int64_t>(*n, 2, 255)}, {"native", Json{{"method", "floor"}}}};
}

std::optional<std::string> write_posterize(const Json& adjust, const core::Page&, std::vector<std::string>& notes) {
    double n = 4;
    try {
        n = native_text(adjust, "method") == "floor" ? std::round(setting(adjust, "levels", 4)) : std::clamp(whole_setting(adjust, "levels", 4), 2.0, 64.0);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    if (!std::isfinite(n)) return std::nullopt;
    if (native_text(adjust, "method") != "floor")
        notes.emplace_back("Genko（Python）の階調化は段の区切り方が Photoshop と違うため、同じ段の数の Photoshop の階調化として書きました");
    std::string out;
    put16(out, static_cast<std::uint32_t>(std::clamp(n, 2.0, 255.0)));
    put16(out, 0);
    return out;
}

std::optional<Json> read_threshold(std::string_view d, std::vector<std::string>& notes) {
    const auto level = be16(d, 0);
    if (!level) {
        note_unread(notes, "thrs", "途中で切れている");
        return std::nullopt;
    }
    return Json{{"kind", "threshold"}, {"threshold", std::clamp<std::int64_t>(*level, 1, 255)}};
}

std::optional<std::string> write_threshold(const Json& adjust, const core::Page&, std::vector<std::string>& notes) {
    const bool bitonal = adjust.value("kind", std::string()) == "bitonal";
    double level = bitonal ? 180 : 128;
    try {
        level = whole_setting(adjust, "threshold", level);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    if (!std::isfinite(level)) return std::nullopt;
    if (bitonal) {  // (白黒にする takes white above the threshold: Photoshop's threshold from the next level)
        level += 1;
        notes.emplace_back("白黒にする（bitonal）の補正は、Photoshop の 2 階調化として書きました");
    }
    std::string out;
    put16(out, static_cast<std::uint32_t>(std::clamp(level, 1.0, 255.0)));
    put16(out, 0);
    return out;
}

std::optional<Json> read_exposure(std::string_view d, std::vector<std::string>& notes) {
    const auto e = bef32(d, 2), o = bef32(d, 6), g = bef32(d, 10);
    if (!e || !o || !g || !std::isfinite(*e) || !std::isfinite(*o) || !std::isfinite(*g)) {
        note_unread(notes, "expA", "途中で切れているか、値が数でない");
        return std::nullopt;
    }
    return Json{{"kind", "exposure"}, {"exposure", number_json(std::clamp(*e, -20.0, 20.0))}, {"offset", number_json(std::clamp(*o, -0.5, 0.5))},
                {"gamma", number_json(std::clamp(*g, 0.01, 9.99))}};
}

std::optional<std::string> write_exposure(const Json& adjust, const core::Page&, std::vector<std::string>&) {
    core::Exposure e;
    try {
        e = core::Exposure::parse(adjust);
    } catch (const core::Error&) {
        return std::nullopt;
    }
    std::string out;
    put16(out, 1);
    put_f32(out, e.stops);
    put_f32(out, e.offset);
    put_f32(out, e.gamma);
    out += '\x01';  // (colour space flag)
    return out;
}

// --- grdm ------------------------------------------------------------------------------------------------------------

using Stop = std::pair<double, std::array<double, 3>>;  // (place 0..1, sRGB 0..1)

double to_linear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double to_encoded(double c) {
    c = std::clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

std::array<double, 3> into_space(std::string_view method, const std::array<double, 3>& rgb) {
    if (method == "Lnr ") return {to_linear(rgb[0]), to_linear(rgb[1]), to_linear(rgb[2])};
    if (method != "Perc") return rgb;
    const double r = to_linear(rgb[0]), g = to_linear(rgb[1]), b = to_linear(rgb[2]);
    const double l = std::cbrt(0.41222146 * r + 0.53633255 * g + 0.051445995 * b);
    const double m = std::cbrt(0.2119035 * r + 0.6806995 * g + 0.10739696 * b);
    const double s = std::cbrt(0.08830246 * r + 0.28171885 * g + 0.6299787 * b);
    return {0.21045426 * l + 0.7936178 * m - 0.004072047 * s, 1.9779985 * l - 2.4285922 * m + 0.4505937 * s,
            0.025904037 * l + 0.78277177 * m - 0.80867577 * s};
}

std::array<double, 3> out_of_space(std::string_view method, const std::array<double, 3>& p) {
    if (method == "Lnr ") return {to_encoded(p[0]), to_encoded(p[1]), to_encoded(p[2])};
    if (method != "Perc") return {std::clamp(p[0], 0.0, 1.0), std::clamp(p[1], 0.0, 1.0), std::clamp(p[2], 0.0, 1.0)};
    const double l = std::pow(p[0] + 0.39633778 * p[1] + 0.21580376 * p[2], 3);
    const double m = std::pow(p[0] - 0.105561346 * p[1] - 0.06385417 * p[2], 3);
    const double s = std::pow(p[0] - 0.08948418 * p[1] - 1.2914855 * p[2], 3);
    return {to_encoded(4.0767417 * l - 3.3077116 * m + 0.23096994 * s), to_encoded(-1.268438 * l + 2.6097574 * m - 0.34131938 * s),
            to_encoded(-0.0041960863 * l - 0.7034186 * m + 1.7076147 * s)};
}

// Photoshop's gradient (its smoothness — a Catmull-Rom spline blended in —, its midpoints, its interpolation method)
// sampled into dense linear stops (PhotoCraft's gradient_bake: fitted to Photoshop's own composites).
std::vector<Stop> baked(std::vector<Stop> stops, const std::vector<double>& mids, double smooth, std::string_view method) {
    const bool plain = std::all_of(mids.begin(), mids.end(), [](double m) { return std::abs(m - 0.5) < 1e-3; });
    std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.first < b.first; });
    if (stops.size() < 2 || (smooth <= 0.0 && method != "Perc" && method != "Lnr " && plain)) return stops;
    std::vector<Stop> pts;
    for (const Stop& s : stops) pts.emplace_back(s.first, into_space(method, s.second));
    const std::size_t n = pts.size();
    smooth = std::clamp(smooth, 0.0, 1.0);
    const auto eval = [&](double t) -> std::array<double, 3> {
        if (t <= pts[0].first) return pts[0].second;
        if (t >= pts[n - 1].first) return pts[n - 1].second;
        std::size_t i = 0;
        for (std::size_t k = 0; k + 1 < n; ++k) {
            if (t >= pts[k].first && t <= pts[k + 1].first) {
                i = k;
                break;
            }
        }
        const double t0 = pts[i].first, t1 = pts[i + 1].first, h = std::max(t1 - t0, 1e-6);
        double u = (t - t0) / h;
        const double mid = std::clamp(i < mids.size() ? mids[i] : 0.5, 0.05, 0.95);
        u = u <= mid ? 0.5 * u / mid : 0.5 + 0.5 * (u - mid) / (1.0 - mid);
        const auto tangent = [&](std::size_t k) {
            const std::size_t a = k == 0 ? 0 : k == n - 1 ? n - 2 : k - 1;
            const std::size_t b = k == 0 ? 1 : k == n - 1 ? n - 1 : k + 1;
            const double d = std::max(pts[b].first - pts[a].first, 1e-6);
            const double half = k == 0 || k == n - 1 ? 0.5 : 1.0;
            return std::array<double, 3>{(pts[b].second[0] - pts[a].second[0]) / d * half, (pts[b].second[1] - pts[a].second[1]) / d * half,
                                         (pts[b].second[2] - pts[a].second[2]) / d * half};
        };
        const auto m0 = tangent(i), m1 = tangent(i + 1);
        const double u2 = u * u, u3 = u2 * u;
        const double h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        std::array<double, 3> out{};
        for (std::size_t c = 0; c < 3; ++c) {
            const double p0 = pts[i].second[c], p1 = pts[i + 1].second[c];
            const double linear = p0 + (p1 - p0) * u;
            const double spline = h00 * p0 + h10 * h * m0[c] + h01 * p1 + h11 * h * m1[c];
            out[c] = linear + (spline - linear) * smooth;
        }
        return out;
    };
    std::vector<Stop> out;
    for (int k = 0; k <= 96; ++k) {
        const double t = k / 96.0;
        out.emplace_back(t, out_of_space(method, eval(t)));
    }
    return out;
}

std::optional<Json> read_gradient(std::string_view d, std::vector<std::string>& notes) {
    const auto fail = [&notes](const char* why) -> std::optional<Json> {
        note_unread(notes, "grdm", why);
        return std::nullopt;
    };
    const auto version = be16(d, 0);
    if (!version || d.size() < 4) return fail("途中で切れている");
    const bool reverse = d[2] != 0, dither = d[3] != 0;
    std::string method;
    std::size_t at = 4;
    if (*version == 3) {
        if (d.size() < 8) return fail("途中で切れている");
        method = std::string(d.substr(4, 4));
        at = 8;
    } else if (*version != 1) {
        return fail("読めない版の");
    }
    const auto name = be32(d, at);
    if (!name || *name > d.size()) return fail("途中で切れている");
    at += 4 + std::size_t(*name) * 2;
    const auto n = be16(d, at);
    if (!n) return fail("途中で切れている");
    at += 2;
    std::vector<Stop> stops;
    std::vector<double> mids;
    for (std::uint32_t i = 0; i < *n; ++i) {
        const auto loc = be32(d, at), mid = be32(d, at + 4), space = be16(d, at + 8);
        const auto r = be16(d, at + 10), g = be16(d, at + 12), b = be16(d, at + 14);
        if (!loc || !mid || !space || !r || !g || !b) return fail("途中で切れている");
        if (*space != 0) return fail("RGB でない色を持つ");
        stops.push_back({std::clamp(*loc / 4096.0, 0.0, 1.0), {*r / 65535.0, *g / 65535.0, *b / 65535.0}});
        mids.push_back(*mid / 100.0);
        at += 20;
    }
    if (stops.size() < 2) return fail("ノイズのグラデーションで、色の分岐点を持たない");
    const auto nt = be16(d, at);
    if (!nt) return fail("途中で切れている");
    at += 2 + std::size_t(*nt) * 10 + 2;  // (the transparency stops, the expansion count)
    const auto smooth = be16(d, at);
    if (!smooth) return fail("途中で切れている");
    // (midpoint k is the segment's after stop k in place order)
    std::vector<std::size_t> order(stops.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&stops](std::size_t a, std::size_t b) { return stops[a].first < stops[b].first; });
    std::vector<double> segment_mids;
    for (std::size_t k = 1; k < order.size(); ++k) segment_mids.push_back(mids[order[k]]);
    const std::vector<Stop> dense = baked(stops, segment_mids, *smooth / 4096.0, method);
    Json list = Json::array();
    for (const Stop& s : dense) {
        list.push_back(Json::array({number_json(std::round(s.first * 10000.0) / 10000.0),
                                    Json::array({q255(s.second[0] * 255), q255(s.second[1] * 255), q255(s.second[2] * 255)})}));
    }
    Json out{{"kind", "gradient_map"}, {"stops", list}};
    if (reverse || dither) {
        Json native = Json::object();
        if (reverse) native["reverse"] = true;
        if (dither) native["dither"] = true;
        out["native"] = native;
    }
    return out;
}

std::optional<std::string> write_gradient(const Json& adjust, const core::Page&, std::vector<std::string>& notes) {
    std::vector<Stop> stops;
    try {
        if (const Json* given = core::get(adjust, "stops"); given != nullptr && core::py_truthy(*given)) {
            for (const Json& st : core::iterate(*given)) {
                const double at = core::to_float(core::subscript(st, 0));
                const std::vector<Json> rgb = core::iterate(core::subscript(st, 1));
                if (rgb.size() < 3) return std::nullopt;
                stops.push_back({at, {static_cast<double>(core::to_int(rgb[0])), static_cast<double>(core::to_int(rgb[1])), static_cast<double>(core::to_int(rgb[2]))}});
            }
        } else {
            const Json* colors = core::get(adjust, "colors");
            const Json list = colors != nullptr && core::py_truthy(*colors) ? *colors : Json::array({Json::array({0, 0, 0}), Json::array({255, 255, 255})});
            const std::vector<Json> all = core::iterate(list);
            for (std::size_t i = 0; i < all.size(); ++i) {
                const std::vector<Json> rgb = core::iterate(all[i]);
                if (rgb.size() < 3) return std::nullopt;
                stops.push_back({all.size() > 1 ? static_cast<double>(i) / static_cast<double>(all.size() - 1) : 0.0,
                                 {static_cast<double>(core::to_int(rgb[0])), static_cast<double>(core::to_int(rgb[1])), static_cast<double>(core::to_int(rgb[2]))}});
            }
        }
    } catch (const core::Error&) {
        note_unwritten(notes, "gradient_map", "色が読めない");
        return std::nullopt;
    }
    if (stops.size() < 2) return std::nullopt;
    std::stable_sort(stops.begin(), stops.end(), [](const Stop& a, const Stop& b) { return a.first < b.first; });
    std::string out;
    put16(out, 1);
    out += static_cast<char>(native_flag(adjust, "reverse") ? 1 : 0);
    out += static_cast<char>(native_flag(adjust, "dither") ? 1 : 0);
    descriptor::put_unicode(out, u"Custom");
    const std::size_t count = std::min<std::size_t>(stops.size(), 0xffff);
    put16(out, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        put32(out, static_cast<std::uint32_t>(std::lround(std::clamp(stops[i].first, 0.0, 1.0) * 4096.0)));
        put32(out, 50);
        put16(out, 0);
        for (const double c : stops[i].second) put16(out, static_cast<std::uint32_t>(std::lround(std::clamp(c, 0.0, 255.0) / 255.0 * 65535.0)));
        put16(out, 0);  // (the fourth component)
        put16(out, 0);
    }
    put16(out, 2);
    for (const std::uint32_t t : {0u, 4096u}) {
        put32(out, t);
        put32(out, 50);
        put16(out, 255);
    }
    put16(out, 2);   // (expansion count)
    put16(out, 0);   // (smoothness)
    put16(out, 32);  // (length)
    put16(out, 0);   // (mode)
    put32(out, 0);   // (random seed)
    put16(out, 0);   // (showing transparency)
    put16(out, 0);   // (using vector colour)
    put32(out, 2048);  // (roughness)
    put16(out, 3);     // (colour model)
    for (const std::uint32_t v : {0u, 0u, 0u, 0u, 0x8000u, 0x8000u, 0x8000u, 0x8000u}) put16(out, v);
    put16(out, 0);
    return out;
}

// --- clrL ------------------------------------------------------------------------------------------------------------

std::optional<Json> read_lookup(std::string_view d, std::vector<std::string>& notes) {
    const auto version = be16(d, 0);
    const auto values = version && *version == 1 ? versioned(d, 2) : std::nullopt;
    if (!values) {
        note_unread(notes, "clrL", "読めない形の");
        return std::nullopt;
    }
    const auto data = text_of(*values, "LUT3DFileData", "tdta");
    if (!data || data->empty()) {
        note_unread(notes, "clrL", "表を持たない（プロファイルによる）");
        return std::nullopt;
    }
    if (current.tables == nullptr) {  // (no book to take its table: not read)
        note_unread(notes, "clrL", "表を置く原稿が無い");
        return std::nullopt;
    }
    const auto format = text_of(*values, "LUTFormat", "enum");
    const char* file = format == std::optional<std::string>("LUTFormat3DL") ? "x.3dl" : format == std::optional<std::string>("LUTFormatLOOK") ? "x.look" : "x.cube";
    core::lut3d::Table table;
    try {
        table = core::lut3d::parse(file, *data);
    } catch (const core::Error&) {
        note_unread(notes, "clrL", "表が読めない");
        return std::nullopt;
    }
    const std::string bytes = core::lut3d::encode(table);
    const std::string ref = core::bytes_ref(bytes);
    (*current.tables)[ref] = std::make_shared<const std::string>(bytes);  // (import_psd takes it into the book)
    Json out{{"kind", "color_lookup"}, {"lut", ref}, {"size", static_cast<std::int64_t>(table.size)}};
    std::string name = text_of(*values, "LUT3DFileName", "TEXT").value_or(text_of(*values, "NM  ", "TEXT").value_or(""));
    if (!name.empty() && name.size() <= 1024) out["name"] = name;
    out["interpolation"] = "trilinear";
    out["dither"] = bool_of(*values, "Dthr").value_or(false);
    return out;
}

std::optional<std::string> write_lookup(const Json& adjust, const core::Page&, std::vector<std::string>& notes) {
    // (a lookup with no table — none chosen — is the identity: a 2³ identity cube, as PhotoCraft writes it)
    core::lut3d::Table table = core::lut3d::identity(2);
    table.title.clear();
    std::string name = adjust.value("name", std::string());
    bool dither = adjust.value("dither", false);
    if (const auto look = adjust.find("look"); look != adjust.end() && look->is_string()) {
        if (const core::lut3d::Table* found = core::lut3d::builtin(look->get<std::string>())) {
            table = *found;
            if (name.empty()) name = table.title;
            table.title.clear();
        }
    } else if (const auto lut = adjust.find("lut"); lut != adjust.end() && lut->is_string()) {
        core::Bytes bytes;  // (the book's own table)
        if (current.doc != nullptr) {
            if (const auto it = current.doc->luts.find(lut->get<std::string>()); it != current.doc->luts.end()) bytes = it->second;
        }
        try {
            if (const auto held = core::lut3d::decoded(lut->get<std::string>(), bytes)) table = *held;
        } catch (const core::Error&) {
            bytes = nullptr;
        }
        if (!bytes) {
            note_unwritten(notes, "color_lookup", "表が原稿に無い");
            return std::nullopt;
        }
    } else {
        dither = false;
    }
    if (adjust.value("interpolation", std::string()) == "tetrahedral")
        notes.emplace_back("カラールックアップの四面体補間は PSD に書けないため、Photoshop では三線形で補間されます");
    descriptor::Writer d;
    d.enumerated("lookupType", "colorLookupType", "3DLUT");
    d.text("NM  ", utf16(name));
    d.boolean("Dthr", dither);
    d.raw("profile", "");
    d.enumerated("LUTFormat", "LUTFormatType", "LUTFormatCUBE");
    d.enumerated("dataOrder", "colorLookupOrder", "rgbOrder");
    d.enumerated("tableOrder", "colorLookupOrder", "bgrOrder");
    d.raw("LUT3DFileData", core::lut3d::write_cube(table));
    d.text("LUT3DFileName", utf16(name));
    std::string out;
    put16(out, 1);
    return out + d.versioned("null");
}

AdjustCodec hooks(std::function<std::optional<Json>(std::string_view, std::vector<std::string>&)> read,
                  std::function<std::optional<std::string>(const Json&, const core::Page&, std::vector<std::string>&)> write) {
    AdjustCodec out;
    out.read = std::move(read);
    out.write = std::move(write);
    return out;
}

}  // namespace

// --- PC2a: the tone kinds (levl, curv, brit, post, thrs, nvrt, expA, grdm) and clrL -----------------------------------

CodecHooks tone_codecs() {
    return {{"levl", hooks(read_levels, write_levels)},
            {"curv", hooks(read_curves, write_curves)},
            {"brit", hooks(read_brightness, write_brightness)},
            {"post", hooks(read_posterize, write_posterize)},
            {"thrs", hooks(read_threshold, write_threshold)},
            {"nvrt", hooks([](std::string_view, std::vector<std::string>&) { return std::optional<Json>(Json{{"kind", "invert"}}); },
                           [](const Json&, const core::Page&, std::vector<std::string>&) { return std::optional<std::string>(std::string()); })},
            {"expA", hooks(read_exposure, write_exposure)},
            {"grdm", hooks(read_gradient, write_gradient)},
            {"clrL", hooks(read_lookup, write_lookup)}};
}

// --- the table and the two ways ---------------------------------------------------------------------------------------

core::Json imported_adjust(const core::Json& adjust, std::string_view blend, const core::Page& page) {
    Json given = adjust;
    if (!blend.empty() && blend != "normal") {  // (its blend first among its native settings)
        Json native{{"blend", std::string(blend)}};
        if (const auto it = given.find("native"); it != given.end() && it->is_object()) {
            for (const auto& [key, value] : it->items()) native[key] = value;
        }
        given["native"] = std::move(native);
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        try {
            return core::normalize_adjust(given, &page);
        } catch (const core::OpError&) {
            // (what the page cannot hold: the blend first, then the channel records)
            auto native = given.find("native");
            if (native == given.end() || !native->is_object()) throw;
            if (attempt == 0) {
                native->erase("blend");
            } else {
                native->erase("channels");
                native->erase("space");
            }
            if (native->empty()) given.erase("native");
        }
    }
    return core::normalize_adjust(given, &page);
}

const AdjustContext& adjust_context() { return current; }

std::string_view adjust_mode(int psd_mode) {
    switch (psd_mode) {
        case 3: return "rgb";
        case 1:
        case 8: return "gray";  // (grayscale, duotone: one channel)
        case 4: return "cmyk";
        case 9: return "lab";
        default: return "other";
    }
}

const std::vector<AdjustCodec>& adjust_codecs() {
    static const std::vector<AdjustCodec> table = make_table();
    return table;
}

namespace {
std::atomic<int> python_views{0};  // (the PythonView scopes alive)
}  // namespace

PythonView::PythonView() { ++python_views; }
PythonView::~PythonView() { --python_views; }

std::string adjust_label(std::string_view key) {
    static const std::vector<std::pair<std::string_view, std::string_view>> labels{
        {"levl", "レベル補正"},     {"curv", "トーンカーブ"},     {"brit", "明るさ・コントラスト"}, {"post", "階調化"},
        {"thrs", "2 値化"},         {"nvrt", "階調の反転"},       {"expA", "露光量"},               {"grdm", "グラデーションマップ"},
        {"clrL", "カラールックアップ"}, {"hue2", "色相・彩度"},   {"vibA", "自然な彩度"},           {"blnc", "カラーバランス"},
        {"blwh", "白黒"},           {"phfl", "レンズフィルター"}, {"mixr", "チャンネルミキサー"},   {"selc", "特定色域の選択"}};
    for (const auto& [k, label] : labels) {
        if (k == key) return std::string(label);
    }
    return std::string(key);
}

std::string kind_label(std::string_view kind) {
    if (kind == "hue") return "色相・彩度・明度";  // (Python's: its own name)
    if (kind == "bitonal") return "白黒にする";
    for (const AdjustCodec& each : adjust_codecs()) {
        if (each.kind == kind) return adjust_label(each.key);
    }
    return std::string(kind);
}

void note_unread(std::vector<std::string>& notes, std::string_view key, std::string_view why) {
    const std::string label = adjust_label(key);
    const std::string named = label == key ? std::string(key) : std::string(key) + ": " + label;
    notes.push_back("PSD の調整レイヤー（" + named + "）は" + std::string(why) + "ため、読み込みませんでした");
}

void note_unwritten(std::vector<std::string>& notes, std::string_view kind, std::string_view why) {
    notes.push_back(kind_label(kind) + "の補正レイヤーは" + std::string(why) + "ため、PSD の調整レイヤーにせず統合画像にだけ入れました");
}

std::optional<core::Json> read_adjustment(std::string_view key, std::string_view payload, std::vector<std::string>& notes, const AdjustContext& context) {
    if (python_views.load() > 0) {
        note_unread(notes, key, "Python 版の読み方で読んでいる");
        return std::nullopt;
    }
    for (const AdjustCodec& each : adjust_codecs()) {
        if (each.key != key) continue;
        if (!each.read) {
            note_unread(notes, key, "この版ではまだ読めない種類の");
            return std::nullopt;
        }
        const AdjustContext was = current;
        current = context;
        try {
            std::optional<core::Json> out = each.read(payload, notes);
            current = was;
            return out;
        } catch (...) {
            current = was;
            throw;
        }
    }
    note_unread(notes, key, "Genko が読める種類でない");
    return std::nullopt;
}

std::optional<AdjustBlock> write_adjustment(const core::Json& adjust, const core::Page& page, std::vector<std::string>& notes, const core::Document* doc) {
    if (python_views.load() > 0) return std::nullopt;  // (Python's view: in the merged picture only)
    const auto kind = adjust.is_object() ? adjust.find("kind") : adjust.end();
    if (!adjust.is_object() || kind == adjust.end() || !kind->is_string()) return std::nullopt;
    // (白黒にする, Python's bitonal: the nearest is Photoshop's threshold)
    const std::string wanted = kind->get<std::string>() == "bitonal" ? std::string("threshold") : kind->get<std::string>();
    for (const AdjustCodec& each : adjust_codecs()) {
        if (each.kind != wanted) continue;
        if (!each.write) return std::nullopt;  // (not written yet: its change is in the merged picture, as Python's PSD)
        const AdjustContext was = current;
        current = AdjustContext{};
        current.doc = doc;
        pending_companion().clear();
        std::optional<std::string> payload;
        try {
            payload = each.write(adjust, page, notes);
        } catch (...) {
            current = was;
            throw;
        }
        current = was;
        if (!payload) return std::nullopt;
        AdjustBlock out{std::string(each.key), std::move(*payload), {}, {}};
        if (!pending_companion().empty()) {  // (brightness_contrast's modern settings: its CgEd block)
            out.companion_key = "CgEd";
            out.companion = std::move(pending_companion());
            pending_companion().clear();
        }
        return out;
    }
    return std::nullopt;
}

}  // namespace genko::render::psd
