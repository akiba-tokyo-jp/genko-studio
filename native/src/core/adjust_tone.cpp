// PC2a (階調の補正・3D LUT): the formulas of the tone corrections, filled into the table of correction kinds
// (core/adjust_kinds.hpp) — levels' and a tone curve's native (the upstream formula: the user's decision 4; the channel
// records in their space), brightness_contrast's and posterize's upstream formulas, a gradient map's reverse and
// dither, and color_lookup (its file made into the book's asset, its table looked up). Equalize looks at the whole
// layer: render/ops_adjust.cpp. PhotoCraft v0.2.0 compose/adjust.rs 55-105, 205-250, 312-316, 424-535 and
// engine/edit_cmds.rs 223-301, engine/extra_cmds.rs 156-187; attribution: native/third_party/photocraft/NOTICE.md.

#include "core/adjust_tone.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#include "core/adjust_kinds.hpp"
#include "core/color_adjust.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/filters.hpp"
#include "core/ink.hpp"
#include "core/lut3d.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"

namespace genko::core {

namespace {

using Curve = std::function<double(double)>;

constexpr std::size_t kLutSize = 4096;  // (PhotoCraft's LUT_SIZE: the upstream formulas sampled so, then interpolated)

std::mutex& bridge_lock() {
    static std::mutex lock;
    return lock;
}
ToneBridgeMaker& bridge_maker() {
    static ToneBridgeMaker maker;
    return maker;
}

// Python's settings of the adjust (without its kind and its native).
Json python_settings(const Json& adjust) {
    Json out = Json::object();
    for (const auto& [key, value] : adjust.items()) {
        if (key != "kind" && key != "native") out[key] = value;
    }
    return out;
}

const Json& native_part(const Json& adjust) {
    static const Json none = Json::object();
    const auto it = adjust.find("native");
    return it != adjust.end() && it->is_object() ? *it : none;
}

std::string native_text(const Json& adjust, std::string_view key) {
    const Json& native = native_part(adjust);
    const auto it = native.find(key);
    return it != native.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool native_flag(const Json& adjust, std::string_view key) {
    const Json& native = native_part(adjust);
    const auto it = native.find(key);
    return it != native.end() && *it == Json(true);
}

// float(x) of a Python setting, or the fallback when it is not given (None counts as not given).
double setting(const Json& settings, std::string_view key, double fallback) {
    const Json* v = get(settings, key);
    return v == nullptr || v->is_null() ? fallback : to_float(*v);
}

// Python's "channel" (r, g, b): only that one of the three, else all.
int python_channel(const Json& settings) {
    const Json* given = get(settings, "channel");
    const std::string name = given != nullptr && py_truthy(*given) ? py_str(*given) : std::string("rgb");
    return name == "r" ? 0 : name == "g" ? 1 : name == "b" ? 2 : -1;
}

unsigned char byte_of(double v) {
    if (std::isnan(v)) return 0;
    return static_cast<unsigned char>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

// --- PhotoCraft's formulas (compose/adjust.rs) ------------------------------------------------------------------------

struct Levels {
    double in_black = 0, in_white = 1, gamma = 1, out_black = 0, out_white = 1;
};

// A record's settings (0..255 as Genko keeps them) in PhotoCraft's ranges (engine/adjust_params.rs: levels).
Levels levels_of(double black, double white, double gamma, double out_black, double out_white) {
    Levels out;
    black = std::clamp(black, 0.0, 253.0);
    white = std::max(std::clamp(white, 2.0, 255.0), black + 2);
    out.in_black = black / 255;
    out.in_white = white / 255;
    out.gamma = std::clamp(gamma, 0.01, 9.99);
    out.out_black = std::clamp(out_black, 0.0, 255.0) / 255;
    out.out_white = std::clamp(out_white, 0.0, 255.0) / 255;
    return out;
}

Levels levels_record(const Json& record) {
    const auto at = [&record](const char* key, double fallback) {
        const auto it = record.find(key);
        return it != record.end() && it->is_number() ? it->get<double>() : fallback;
    };
    return levels_of(at("black", 0), at("white", 255), at("gamma", 1), at("out_black", 0), at("out_white", 255));
}

double levels_value(const Levels& ch, double v) {
    const double range = std::max(ch.in_white - ch.in_black, 1e-6);
    const double u = std::clamp((v - ch.in_black) / range, 0.0, 1.0);
    const double g = std::max(ch.gamma, 0.01);
    double t = 0;
    if (g > 1.0) {
        // (a midtone gamma > 1 lifts the shadows; Photoshop bounds the power curve's infinite slope at black — a soft
        // toe of initial slope 2^gamma: the p-norm soft minimum of the two, the power curve itself in the body)
        const double power = std::pow(u, 1.0 / g);
        const double line = std::pow(2.0, g) * u;
        if (power <= 1e-6 || line <= 1e-6) {
            t = std::min(power, line);
        } else {
            const double p = 1.8 + 8.8 / g;
            t = std::pow(std::pow(power, -p) + std::pow(line, -p), -1.0 / p);
        }
    } else {
        t = std::pow(u, 1.0 / g);
    }
    return ch.out_black + t * (ch.out_white - ch.out_black);
}

std::vector<double> sampled(const Curve& f) {
    std::vector<double> out(kLutSize);
    for (std::size_t k = 0; k < kLutSize; ++k) out[k] = f(static_cast<double>(k) / (kLutSize - 1));
    return out;
}

double modern_brightness(double v, double brightness) {
    if (brightness == 0.0) return v;
    const double s = std::pow(1.375, brightness / 50.0);
    const double p = brightness >= 0.0 ? std::max(4.5 - 0.013 * brightness, 2.0) : 5.0 - 0.072 * brightness;
    v = std::clamp(v, 0.0, 1.0);
    return std::clamp(s * v + (1.0 - s) * std::pow(v, p), 0.0, 1.0);
}

double modern_contrast(double v, double contrast) {
    if (contrast == 0.0) return v;
    const double k = contrast / 128.0;
    const double end = 1.0 - k, mid = 1.0 + k;
    // (a cubic Hermite on [0, 0.5]: (0, 0) of slope `end`, (0.5, 0.5) of slope `mid`)
    const auto half = [end, mid](double t) {
        return end * 0.5 * (t * t * t - 2.0 * t * t + t) + (-2.0 * t * t * t + 3.0 * t * t) * 0.5 + mid * 0.5 * (t * t * t - t * t);
    };
    v = std::clamp(v, 0.0, 1.0);
    return v <= 0.5 ? half(v / 0.5) : 1.0 - half((1.0 - v) / 0.5);
}

double posterize_floor(double v, double levels) {
    const double n = std::clamp(levels, 2.0, 255.0);
    const double x = std::round((std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0)) * 255.0);
    const double bin = std::min(std::floor(x * n / 256.0), n - 1.0);
    return std::floor(bin * 255.0 / (n - 1.0)) / 255.0;
}

// ImageOps.grayscale's weights (as PreciseAdjustment's gradient map takes the lightness), 0..1.
double lightness(const std::array<double, 3>& rgb) {
    return std::clamp((rgb[0] * 255 * 19595 + rgb[1] * 255 * 38470 + rgb[2] * 255 * 7471) / 65536.0, 0.0, 255.0) / 255.0;
}

// --- the functions ------------------------------------------------------------------------------------------------------

// Each channel of a space through its own curve (0..1 → 0..1). rgb: red, green, blue of the colour; gray: the one curve
// on each of them; cmyk and lab: the colour's tones in that space (the page's own plates: its records, ink_records), on
// another page through the bridge — the change added as the difference of two round trips (PhotoCraft's tone_in_space),
// so what a curve leaves alone stays exact; each colour through the bridge once (a colour met again is not converted
// again: a page of few colours is not converted pixel by pixel).
class ChannelFunction : public AdjustFunction {
public:
    ChannelFunction(std::string space, std::vector<Curve> rows) : space_(std::move(space)), rows_(std::move(rows)) {}

    void set_tables(std::array<std::array<std::uint8_t, 256>, 3> t) { tables_ = ChannelTables{t}; }
    void set_bridge(ToneBridge bridge) { bridge_ = std::move(bridge); }
    void set_dither(bool dither) { dither_ = dither; }

    std::array<double, 3> apply(const std::array<double, 3>& rgb, int x, int y) const override {
        std::array<double, 3> out = through(rgb);
        if (dither_) {
            const double d = lut3d::bayer4(x, y) / 255.0;
            for (double& v : out) v = std::clamp(v + d, 0.0, 1.0);
        }
        return out;
    }
    bool positional() const override { return dither_; }
    std::optional<ChannelTables> tables() const override { return dither_ ? std::nullopt : tables_; }
    std::optional<InkRecords> ink_records() const override {
        if (space_ == "rgb" || dither_) return std::nullopt;
        InkRecords out;
        out.space = space_ == "cmyk" ? InkSpace::Cmyk : space_ == "lab" ? InkSpace::Lab : InkSpace::Gray;
        out.plates.assign(rows_.begin(), rows_.end());
        return out;
    }

private:
    std::array<double, 3> through(const std::array<double, 3>& rgb) const {
        if (space_ == "rgb") return {rows_[0](rgb[0]), rows_[1](rgb[1]), rows_[2](rgb[2])};
        if (space_ == "gray") return {rows_[0](rgb[0]), rows_[0](rgb[1]), rows_[0](rgb[2])};
        if (!bridge_.into || !bridge_.back) adjust_not_yet("the channel records of the space " + space_ + " on this page", "PC2a");
        Key key{};
        std::memcpy(key.data(), rgb.data(), sizeof key);
        {
            const std::lock_guard<std::mutex> hold(known_lock_);
            if (const auto it = known_.find(key); it != known_.end()) return it->second;
        }
        const std::array<double, 3> out = bridged(rgb);
        const std::lock_guard<std::mutex> hold(known_lock_);
        if (known_.size() >= kKnownMost) known_.clear();  // (a picture of very many colours: the latest kept)
        known_.emplace(key, out);
        return out;
    }

    // One colour into the space, through the curves and back (the change added: the difference of two round trips).
    std::array<double, 3> bridged(const std::array<double, 3>& rgb) const {
        const std::vector<double> tones = bridge_.into(rgb);
        std::vector<double> changed = tones;
        for (std::size_t i = 0; i < changed.size() && i < rows_.size(); ++i) changed[i] = rows_[i](tones[i]);
        if (changed == tones) return rgb;
        const std::array<double, 3> before = bridge_.back(tones), after = bridge_.back(changed);
        return {std::clamp(rgb[0] + after[0] - before[0], 0.0, 1.0), std::clamp(rgb[1] + after[1] - before[1], 0.0, 1.0),
                std::clamp(rgb[2] + after[2] - before[2], 0.0, 1.0)};
    }

    using Key = std::array<std::uint64_t, 3>;  // (a colour's three doubles, bit for bit)
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::uint64_t h = 1469598103934665603ULL;
            for (const std::uint64_t v : k) h = (h ^ v) * 1099511628211ULL;
            return static_cast<std::size_t>(h ^ (h >> 29));
        }
    };
    static constexpr std::size_t kKnownMost = std::size_t{1} << 18;

    std::string space_;
    std::vector<Curve> rows_;
    std::optional<ChannelTables> tables_;
    ToneBridge bridge_;
    bool dither_ = false;
    mutable std::mutex known_lock_;
    mutable std::unordered_map<Key, std::array<double, 3>, KeyHash> known_;
};

// The 8-bit tables of three curves (each value v / 255 through its curve, rounded).
std::array<std::array<std::uint8_t, 256>, 3> tables_of(const std::vector<Curve>& rows, bool gray) {
    std::array<std::array<std::uint8_t, 256>, 3> out{};
    for (std::size_t c = 0; c < 3; ++c) {
        const Curve& f = rows[gray ? 0 : c];
        for (int i = 0; i < 256; ++i) out[c][static_cast<std::size_t>(i)] = byte_of(f(i / 255.0));
    }
    return out;
}

std::shared_ptr<ChannelFunction> made(std::string space, std::vector<Curve> rows, const Page& page) {
    const bool flat = space == "rgb" || space == "gray";
    auto f = std::make_shared<ChannelFunction>(space, rows);
    if (flat) f->set_tables(tables_of(rows, space == "gray"));
    // (records in another space than the page's: through the drawing's conversions — a page of that space lays them
    // on its plates themselves, ink_records)
    if (!flat) {
        const std::lock_guard<std::mutex> hold(bridge_lock());
        if (bridge_maker()) f->set_bridge(bridge_maker()(page, space));
    }
    return f;
}

// A Python composite as one curve per channel of a space: rgb its own red, green, blue (Python's "channel" kept);
// another space each plate's tone through it (its "channel" not looked at).
std::vector<Curve> python_composite(const std::string& kind, const Json& settings, std::size_t channels, bool rgb) {
    Json all = settings;
    if (!rgb) all.erase("channel");
    auto f = std::make_shared<PreciseAdjustment>(kind, all);
    std::vector<Curve> out;
    for (std::size_t c = 0; c < channels; ++c) {
        const std::size_t at = rgb ? c : 0;
        out.push_back([f, at](double v) { return (*f)({v, v, v})[at]; });
    }
    return out;
}

// levels and curve: the composite (Python's settings) and the channel records in their space, by Genko's formulas
// (Python's levels_table and monotone cubic: method genko) or PhotoCraft's (upstream) — levels the composite then the
// channel's record (PhotoCraft adjust.rs:211), a curve the channel's record then the composite (216).
AdjustPtr tone_function(const Json& adjust, const Page& page, const LookupTables*) {
    const std::string kind = adjust.value("kind", std::string());
    const Json settings = python_settings(adjust);
    const bool upstream = native_text(adjust, "method") == "upstream";
    const Json& native = native_part(adjust);
    const auto records_it = native.find("channels");
    const bool has_records = records_it != native.end() && records_it->is_object() && !records_it->empty();
    const std::string space = has_records ? native_text(adjust, "space") : std::string("rgb");
    if (space.empty()) return nullptr;
    const std::vector<std::string_view> names = tone_channels(space);
    const bool rgb = space == "rgb";
    const bool levels = kind == "levels";
    try {
        // the composite on each channel of the space (lab: on the lightness only — PhotoCraft has none there)
        std::vector<Curve> composite;
        if (!upstream) {
            composite = python_composite(kind, settings, names.size(), rgb);
        } else {
            const int only = rgb ? python_channel(settings) : -1;
            Curve one;
            if (levels) {
                const Levels m = levels_of(setting(settings, "black", 0), setting(settings, "white", 255), setting(settings, "gamma", 1),
                                           setting(settings, "out_black", 0), setting(settings, "out_white", 255));
                one = [m](double v) { return levels_value(m, v); };
            } else if (const Json* points = get(settings, "points"); points != nullptr && py_truthy(*points)) {
                // (Python's reading of the points: x rounded, a later point at the same x wins, sorted)
                std::vector<std::array<double, 2>> pts;
                for (const Json& item : iterate(*points)) {
                    const std::vector<Json> xy = iterate(item);
                    if (xy.size() != 2) return nullptr;
                    const double x = py_round_whole(to_float(xy[0])), y = to_float(xy[1]);
                    const auto at = std::find_if(pts.begin(), pts.end(), [x](const auto& p) { return p[0] == x; });
                    if (at != pts.end()) (*at)[1] = y;
                    else pts.push_back({x, y});
                }
                if (pts.size() < 2) return nullptr;
                auto table = std::make_shared<const std::vector<double>>(upstream_curve(pts));
                one = [table](double v) { return upstream_lut(*table, v); };
            } else {  // (no points: Python's gamma curve, in both formulas)
                const double gamma = std::clamp(setting(settings, "gamma", 1.6), 0.2, 5.0);
                one = [gamma](double v) { return std::pow(std::clamp(v, 0.0, 1.0), gamma); };
            }
            for (std::size_t c = 0; c < names.size(); ++c) {
                const bool on = space == "lab" ? c == 0 : only < 0 || static_cast<int>(c) == only;
                composite.push_back(on ? one : Curve([](double v) { return v; }));
            }
        }
        if (space == "lab" && !upstream) {  // (Genko's composite on the lightness only, as upstream's)
            for (std::size_t c = 1; c < composite.size(); ++c) composite[c] = [](double v) { return v; };
        }
        // each channel's record
        std::vector<Curve> rows;
        for (std::size_t c = 0; c < names.size(); ++c) {
            const Json* record = has_records && records_it->contains(names[c]) ? &(*records_it)[std::string(names[c])] : nullptr;
            Curve own;
            if (record == nullptr) {
                own = [](double v) { return v; };
            } else if (upstream) {
                if (levels) {
                    const Levels r = levels_record(*record);
                    own = [r](double v) { return levels_value(r, v); };
                } else {
                    std::vector<std::array<double, 2>> pts;
                    for (const Json& p : *record) pts.push_back({p[0].get<double>(), p[1].get<double>()});
                    auto table = std::make_shared<const std::vector<double>>(upstream_curve(pts));
                    own = [table](double v) { return upstream_lut(*table, v); };
                }
            } else {
                Json params = levels ? Json{{"black", 0}, {"white", 255}, {"gamma", 1.0}, {"out_black", 0}, {"out_white", 255}}
                                     : Json{{"points", *record}};
                if (levels) {
                    for (const auto& [key, value] : record->items()) params[key] = value;
                }
                auto f = std::make_shared<PreciseAdjustment>(kind, params);
                own = [f](double v) { return (*f)({v, v, v})[0]; };
            }
            const Curve& m = composite[c];
            if (upstream) {  // (PhotoCraft samples the chain at 4096 and interpolates it)
                auto table = std::make_shared<std::vector<double>>(levels ? sampled([own, m](double v) { return own(m(v)); }) : std::vector<double>());
                if (!levels) {
                    const std::vector<double> mt = sampled(m), ot = sampled(own);
                    table->resize(kLutSize);
                    for (std::size_t k = 0; k < kLutSize; ++k) (*table)[k] = upstream_lut(mt, ot[k]);
                }
                rows.push_back([table](double v) { return upstream_lut(*table, v); });
            } else if (levels) {
                rows.push_back([own, m](double v) { return own(m(v)); });
            } else {
                rows.push_back([own, m](double v) { return m(own(v)); });
            }
        }
        auto f = made(space, rows, page);
        if (!upstream && (rgb || space == "gray")) {
            // (the 8-bit way of Genko's formulas: Python's own tables, the composite's and each record's, one after the
            // other — the same bytes as Python's table where the records leave a channel alone)
            const Adjustment comp = adjustment(kind, rgb ? settings : [&] { Json s = settings; s.erase("channel"); return s; }());
            std::array<std::array<std::uint8_t, 256>, 3> t{};
            for (std::size_t c = 0; c < 3; ++c) {
                const std::size_t ch = rgb ? c : 0;
                const Json* record = has_records && records_it->contains(names[ch]) ? &(*records_it)[std::string(names[ch])] : nullptr;
                std::vector<int> own(256);
                for (int i = 0; i < 256; ++i) own[static_cast<std::size_t>(i)] = i;
                if (record != nullptr) {
                    Json params = levels ? Json{{"black", 0}, {"white", 255}, {"gamma", 1.0}, {"out_black", 0}, {"out_white", 255}}
                                         : Json{{"points", *record}};
                    if (levels) {
                        for (const auto& [key, value] : record->items()) params[key] = value;
                    }
                    own = adjustment(kind, params).tables[0];
                }
                const std::vector<int>& m = comp.tables[ch];
                for (int i = 0; i < 256; ++i) {
                    const int v = levels ? own[static_cast<std::size_t>(std::clamp(m[static_cast<std::size_t>(i)], 0, 255))]
                                         : m[static_cast<std::size_t>(std::clamp(own[static_cast<std::size_t>(i)], 0, 255))];
                    t[c][static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
                }
            }
            f->set_tables(t);
        }
        return f;
    } catch (const PyValueError&) {
        return nullptr;  // (Python's settings it cannot read: the layer does nothing, as Python's)
    } catch (const PyTypeError&) {
        return nullptr;
    } catch (const OpKeyError&) {
        return nullptr;
    } catch (const PyUncaught&) {
        return nullptr;
    }
}

// The same curve on red, green and blue (Python's "channel": only that one).
AdjustPtr per_channel(const Json& settings, Curve one, const Page& page) {
    const int only = python_channel(settings);
    std::vector<Curve> rows;
    for (int c = 0; c < 3; ++c) rows.push_back(only < 0 || c == only ? one : Curve([](double v) { return v; }));
    return made("rgb", std::move(rows), page);
}

// brightness_contrast's upstream formulas (native.method modern | legacy): brightness -150..150, contrast modern
// -50..100, legacy -100..100 (clamped, as PhotoCraft's fields).
AdjustPtr brightness_function(const Json& adjust, const Page& page, const LookupTables*) {
    const Json settings = python_settings(adjust);
    try {
        const bool legacy = native_text(adjust, "method") == "legacy";
        if (!std::isfinite(setting(settings, "brightness", 0)) || !std::isfinite(setting(settings, "contrast", 0))) return nullptr;
        const double brightness = std::clamp(setting(settings, "brightness", 0), -150.0, 150.0);
        if (legacy) {
            const double b = brightness / 255.0;
            const double c = std::clamp(std::clamp(setting(settings, "contrast", 0), -100.0, 100.0), -100.0, 99.0);
            const double k = c >= 0.0 ? 1.0 / (1.0 - c / 100.0) : 1.0 + c / 100.0;
            return per_channel(settings, [b, k](double v) { return std::clamp((v - 0.5) * k + 0.5 + b, 0.0, 1.0); }, page);
        }
        const double contrast = std::clamp(setting(settings, "contrast", 0), -50.0, 100.0);
        return per_channel(settings, [brightness, contrast](double v) {
            return std::clamp(modern_contrast(modern_brightness(v, brightness), contrast), 0.0, 1.0);
        }, page);
    } catch (const PyValueError&) {
        return nullptr;
    } catch (const PyTypeError&) {
        return nullptr;
    }
}

// posterize's upstream bins (native.method floor): 2..255 levels (rounded).
AdjustPtr posterize_function(const Json& adjust, const Page& page, const LookupTables*) {
    const Json settings = python_settings(adjust);
    try {
        const double given = setting(settings, "levels", 4);
        if (!std::isfinite(given)) return nullptr;  // (as Python's int() of it: the layer does nothing)
        const double n = std::clamp(std::round(given), 2.0, 255.0);
        return per_channel(Json::object(), [n](double v) { return posterize_floor(v, n); }, page);
    } catch (const PyValueError&) {
        return nullptr;
    } catch (const PyTypeError&) {
        return nullptr;
    }
}

// A gradient map whose lightness runs the other way (native.reverse) or whose colours are dithered (native.dither,
// PhotoCraft's 4×4 Bayer ±½ of a level by the page's pixel): Python's gradient (its stops or colours) otherwise.
class GradientFunction : public AdjustFunction {
public:
    GradientFunction(const Json& settings, bool reverse, bool dither)
        : map_(std::make_shared<PreciseAdjustment>("gradient_map", settings)), reverse_(reverse), dither_(dither) {}
    std::array<double, 3> apply(const std::array<double, 3>& rgb, int x, int y) const override {
        double t = lightness(rgb);
        if (reverse_) t = 1.0 - t;
        std::array<double, 3> out = (*map_)({t, t, t});  // (a grey's lightness is its value: the gradient at t)
        if (dither_) {
            const double d = lut3d::bayer4(x, y) / 255.0;
            for (double& v : out) v = std::clamp(v + d, 0.0, 1.0);
        }
        return out;
    }
    bool positional() const override { return dither_; }

private:
    std::shared_ptr<PreciseAdjustment> map_;
    bool reverse_, dither_;
};

AdjustPtr gradient_function(const Json& adjust, const Page&, const LookupTables*) {
    try {
        return std::make_shared<GradientFunction>(python_settings(adjust), native_flag(adjust, "reverse"), native_flag(adjust, "dither"));
    } catch (const PyValueError&) {
        return nullptr;
    } catch (const PyTypeError&) {
        return nullptr;
    } catch (const OpKeyError&) {
        return nullptr;
    } catch (const PyUncaught&) {
        return nullptr;
    }
}

// --- color_lookup ---------------------------------------------------------------------------------------------------

class LookupFunction : public AdjustFunction {
public:
    LookupFunction(std::shared_ptr<const lut3d::Table> table, bool tetrahedral, bool dither)
        : table_(std::move(table)), tetrahedral_(tetrahedral), dither_(dither) {}
    std::array<double, 3> apply(const std::array<double, 3>& rgb, int x, int y) const override {
        std::array<double, 3> out = table_ ? lut3d::sample(*table_, rgb, tetrahedral_) : rgb;
        if (dither_) {
            const double d = lut3d::bayer4(x, y) / 255.0;
            for (double& v : out) v = std::clamp(v + d, 0.0, 1.0);
        }
        return out;
    }
    bool positional() const override { return dither_; }

private:
    std::shared_ptr<const lut3d::Table> table_;
    bool tetrahedral_, dither_;
};

std::string base_name(std::string_view path) {
    const std::size_t at = path.find_last_of("/\\");
    return std::string(at == std::string_view::npos ? path : path.substr(at + 1));
}

// An op's color_lookup: a file's text (`data`, its format by `file_name`) made into a table (its asset ref in `lut`,
// its size and name; the table into `made`, which the op keeps in its book or a filter uses), else a look, else a table
// the book holds already (`lut`), else none (the colours as they are). The keys in the table's order.
Json lookup_normalize(Json read, const Page*, LookupTables* made) {
    if (const auto data = read.find("data"); data != read.end()) {
        if (!data->is_string()) throw OpError("adjust color_lookup.data must be the text of a .cube, .3dl or .look file");
        if (made == nullptr) throw OpError("adjust color_lookup.data is an op's (a file's text made into the book's table)");
        const std::string& text = data->get_ref<const std::string&>();
        if (text.size() > lut3d::kMaxFileBytes) throw OpError("adjust color_lookup.data is larger than 64 MiB");
        const std::string file = read.contains("file_name") ? read["file_name"].get<std::string>() : std::string("lut.cube");
        lut3d::Table table;
        try {
            table = lut3d::parse(file, text);
        } catch (const Error& error) {
            throw OpError("adjust color_lookup.data: " + std::string(error.what()));
        }
        const std::string bytes = lut3d::encode(table);
        const std::string ref = bytes_ref(bytes);
        (*made)[ref] = std::make_shared<const std::string>(bytes);
        if (!read.contains("name")) read["name"] = base_name(file);
        read.erase("look");
        read["lut"] = ref;
        read["size"] = static_cast<std::int64_t>(table.size);
    } else if (read.contains("look")) {
        read.erase("lut");
        read.erase("size");
    } else if (const auto lut = read.find("lut"); lut != read.end()) {
        const bool ref = lut->is_string() && lut->get_ref<const std::string&>().size() == 71 && lut->get_ref<const std::string&>().starts_with("sha256:");
        if (!ref) throw OpError("adjust color_lookup.lut must be a lookup table's asset ref (sha256:…)");
    }
    read.erase("data");
    read.erase("file_name");
    if (!read.contains("lut")) read.erase("size");
    Json out = Json::object();
    for (const AdjustParam& p : adjust_kind("color_lookup")->params) {
        if (const auto it = read.find(p.key); it != read.end()) out[std::string(p.key)] = *it;
    }
    return out;
}

AdjustPtr lookup_function(const Json& adjust, const Page&, const LookupTables* luts) {
    const bool tetrahedral = adjust.value("interpolation", std::string()) == "tetrahedral";
    const bool dither = adjust.value("dither", false);
    std::shared_ptr<const lut3d::Table> table;
    if (const auto look = adjust.find("look"); look != adjust.end() && look->is_string()) {
        const lut3d::Table* found = lut3d::builtin(look->get<std::string>());
        if (found == nullptr) return nullptr;
        table = std::shared_ptr<const lut3d::Table>(std::shared_ptr<const lut3d::Table>(), found);
    } else if (const auto lut = adjust.find("lut"); lut != adjust.end() && lut->is_string()) {
        const std::string& ref = lut->get_ref<const std::string&>();
        Bytes bytes;  // (the book's own table, or the one a filter's op just made: never another book's)
        if (luts != nullptr) {
            if (const auto it = luts->find(ref); it != luts->end()) bytes = it->second;
        }
        try {
            table = lut3d::decoded(ref, bytes);
        } catch (const Error&) {
            return nullptr;  // (a table that cannot be read: the book is read-only, its problem reported where it was read)
        }
        if (!table) return nullptr;  // (not held: the same)
    }
    return std::make_shared<LookupFunction>(std::move(table), tetrahedral, dither);
}

AdjustHooks hooks(std::function<AdjustPtr(const Json&, const Page&, const LookupTables*)> function,
                  std::function<Json(Json, const Page*, LookupTables*)> normalize = nullptr) {
    AdjustHooks out;
    out.function = std::move(function);
    out.normalize = std::move(normalize);
    return out;
}

}  // namespace

void set_tone_bridge(ToneBridgeMaker maker) {
    const std::lock_guard<std::mutex> hold(bridge_lock());
    bridge_maker() = std::move(maker);
}

double upstream_levels(const Json& record, double v) { return levels_value(levels_record(record), v); }

std::vector<double> upstream_curve(const std::vector<std::array<double, 2>>& points) {
    std::vector<std::pair<double, double>> pts;
    for (const auto& p : points) pts.emplace_back(std::clamp(p[0], 0.0, 255.0) / 255.0, std::clamp(p[1], 0.0, 255.0) / 255.0);
    std::stable_sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<double, double>> kept;  // (points at the same place: the first)
    for (const auto& p : pts) {
        if (!kept.empty() && std::abs(p.first - kept.back().first) < 1e-6) continue;
        kept.push_back(p);
    }
    if (kept.size() < 2) return sampled([](double v) { return v; });
    const std::size_t n = kept.size();
    // (the natural spline's second derivatives: a tridiagonal solve)
    std::vector<double> m2(n, 0.0);
    if (n > 2) {
        std::vector<double> c(n, 0.0), d(n, 0.0);
        for (std::size_t i = 1; i + 1 < n; ++i) {
            const double h0 = kept[i].first - kept[i - 1].first, h1 = kept[i + 1].first - kept[i].first;
            const double a = h0 / 6.0, b = (h0 + h1) / 3.0, cc = h1 / 6.0;
            const double r = (kept[i + 1].second - kept[i].second) / h1 - (kept[i].second - kept[i - 1].second) / h0;
            const double denom = b - a * c[i - 1];
            c[i] = cc / denom;
            d[i] = (r - a * d[i - 1]) / denom;
        }
        for (std::size_t i = n - 2; i >= 1; --i) {
            m2[i] = d[i] - c[i] * m2[i + 1];
            if (i == 1) break;
        }
    }
    std::vector<double> out(kLutSize);
    for (std::size_t k = 0; k < kLutSize; ++k) {
        const double x = static_cast<double>(k) / (kLutSize - 1);
        if (x <= kept.front().first) {
            out[k] = std::clamp(kept.front().second, 0.0, 1.0);
            continue;
        }
        if (x >= kept.back().first) {
            out[k] = std::clamp(kept.back().second, 0.0, 1.0);
            continue;
        }
        std::size_t i = 0;
        while (i + 2 < n && !(x <= kept[i + 1].first)) ++i;
        const double x0 = kept[i].first, y0 = kept[i].second, x1 = kept[i + 1].first, y1 = kept[i + 1].second;
        const double h = x1 - x0;
        const double a = (x1 - x) / h, b = (x - x0) / h;
        const double y = a * y0 + b * y1 + ((a * a * a - a) * m2[i] + (b * b * b - b) * m2[i + 1]) * h * h / 6.0;
        out[k] = std::clamp(y, 0.0, 1.0);
    }
    return out;
}

double upstream_lut(const std::vector<double>& table, double v) {
    if (table.empty()) return v;
    const double x = (std::isnan(v) ? 0.0 : std::clamp(v, 0.0, 1.0)) * static_cast<double>(table.size() - 1);
    const std::size_t i = std::min(static_cast<std::size_t>(std::floor(x)), table.size() - 1);
    const std::size_t j = std::min(i + 1, table.size() - 1);
    const double f = x - static_cast<double>(i);
    return table[i] * (1.0 - f) + table[j] * f;
}

Json auto_levels(const std::array<std::array<std::uint64_t, 256>, 3>& histograms, std::string_view mode) {
    constexpr double kClip = 0.001;  // (0.1% at each end: Photoshop's default)
    // black and white points with `clip` clipped at each end (edit_cmds.rs: clip_points)
    const auto points = [](const std::array<std::uint64_t, 256>& h) {
        std::uint64_t total = 0;
        for (const std::uint64_t v : h) total += v;
        if (total == 0) return std::pair<int, int>{0, 255};
        const auto lim = static_cast<std::uint64_t>(static_cast<double>(total) * kClip);
        std::uint64_t acc = 0;
        int lo = 0;
        while (lo < 254 && acc + h[static_cast<std::size_t>(lo)] <= lim) acc += h[static_cast<std::size_t>(lo++)];
        acc = 0;
        int hi = 255;
        while (hi > lo + 1 && acc + h[static_cast<std::size_t>(hi)] <= lim) acc += h[static_cast<std::size_t>(hi--)];
        return std::pair<int, int>{lo, hi};
    };
    const auto median = [](const std::array<std::uint64_t, 256>& h) {
        std::uint64_t total = 0;
        for (const std::uint64_t v : h) total += v;
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < 256; ++i) {
            acc += h[i];
            if (acc * 2 >= total) return static_cast<double>(i) / 255.0;
        }
        return 0.5;
    };
    std::array<std::pair<int, int>, 3> at{};
    std::array<double, 3> gamma{1.0, 1.0, 1.0};
    if (mode == "contrast") {  // (the same points for every channel: the colours kept)
        std::array<std::uint64_t, 256> all{};
        for (const auto& h : histograms)
            for (std::size_t i = 0; i < 256; ++i) all[i] += h[i];
        at.fill(points(all));
    } else {
        for (std::size_t c = 0; c < 3; ++c) at[c] = points(histograms[c]);
        if (mode == "color") {  // (each channel's median moved onto the medians' mean: the midtones made neutral)
            std::array<double, 3> meds{};
            for (std::size_t c = 0; c < 3; ++c) meds[c] = median(histograms[c]);
            const double target = (meds[0] + meds[1] + meds[2]) / 3.0;
            for (std::size_t c = 0; c < 3; ++c) {
                const double lo = at[c].first / 255.0, hi = at[c].second / 255.0;
                const double t = std::clamp((meds[c] - lo) / std::max(hi - lo, 1e-3), 0.02, 0.98);
                const double want = std::clamp((target - lo) / std::max(hi - lo, 1e-3), 0.02, 0.98);
                gamma[c] = std::clamp(std::log(t) / std::log(want), 0.1, 9.99);  // (out = t^(1/g) = want)
            }
        }
    }
    Json out = Json::object();
    constexpr std::array<const char*, 3> kNames{"red", "green", "blue"};
    for (std::size_t c = 0; c < 3; ++c) {
        // (a record's own ranges: black 0..253, white at least black + 2)
        const int black = std::min(at[c].first, 253);
        const int white = std::clamp(std::max(at[c].second, black + 2), 2, 255);
        const double g = std::round(gamma[c] * 1000.0) / 1000.0;
        out[kNames[c]] = Json{{"black", black}, {"white", white}, {"gamma", g == std::floor(g) ? Json(static_cast<std::int64_t>(g)) : Json(g)},
                              {"out_black", 0}, {"out_white", 255}};
    }
    return out;
}

std::optional<std::array<double, 256>> equalize_table(const std::array<std::uint64_t, 256>& counts) {
    double total = 0.0;
    for (const std::uint64_t v : counts) total += static_cast<double>(v);
    if (total == 0.0) return std::nullopt;
    double first = 0.0;
    for (const std::uint64_t v : counts) {
        if (v > 0) {
            first = static_cast<double>(v);
            break;
        }
    }
    std::array<double, 256> out{};
    double acc = 0.0;
    for (std::size_t i = 0; i < 256; ++i) {
        acc += static_cast<double>(counts[i]);
        out[i] = total > first ? std::clamp((acc - first) / (total - first), 0.0, 1.0) : static_cast<double>(i) / 255.0;
    }
    return out;
}

HookTable tone_hooks() {
    return {{"levels", hooks(tone_function)},
            {"curve", hooks(tone_function)},
            {"brightness_contrast", hooks(brightness_function)},
            {"posterize", hooks(posterize_function)},
            {"gradient_map", hooks(gradient_function)},
            {"color_lookup", hooks(lookup_function, lookup_normalize)}};
}

}  // namespace genko::core
