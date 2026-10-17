// PC2b (色の補正): the formulas of the colour corrections, filled into the table of correction kinds
// (core/adjust_kinds.hpp) — hue_saturation, vibrance, color_balance, black_white, photo_filter, channel_mixer,
// selective_color and desaturate (Hue/Saturation's saturation −100). PhotoCraft v0.2.0 compose/src/adjust.rs (106-199,
// 252-310, 357-397, 556-594), doc/src/adjust.rs (HueRange), engine/src/adjust_params.rs (PHOTO_FILTERS, the readings),
// engine/src/commands.rs:719-732 (desaturate); attribution and the upstream MIT license:
// native/third_party/photocraft/NOTICE.md.
//
// Each is one AdjustFunction on the page's encoded RGB values (core/adjust.hpp): no tables (a colour's channels mix),
// no records on a page's plates (a CMYK, Lab or grey page works the colours its inks mean and takes them back by its
// rules). Where PhotoCraft holds a result to 0..1 so does this; settings that change nothing leave every value as it is
// (an HDR one too).

#include "core/adjust_color.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/adjust_kinds.hpp"
#include "core/command_bus.hpp"

namespace genko::core {

namespace color_adjust {

namespace {

// Rust's rem_euclid for a positive divisor.
double wrapped(double a, double b) {
    const double r = std::fmod(a, b);
    return r < 0 ? r + b : r;
}

}  // namespace

const std::vector<PhotoFilterPreset>& photo_filters() {
    static const std::vector<PhotoFilterPreset> all{
        {"warming85", {236, 138, 0}}, {"warmingLBA", {250, 150, 0}}, {"warming81", {235, 177, 19}}, {"cooling80", {0, 109, 255}},
        {"coolingLBB", {0, 93, 255}}, {"cooling82", {0, 181, 255}},  {"red", {234, 26, 26}},        {"orange", {243, 132, 23}},
        {"yellow", {249, 227, 28}},   {"green", {25, 201, 25}},      {"cyan", {29, 203, 234}},      {"blue", {29, 53, 234}},
        {"violet", {155, 29, 234}},   {"magenta", {227, 24, 227}},   {"sepia", {172, 122, 51}},     {"deepRed", {255, 0, 0}},
        {"deepBlue", {0, 34, 205}},   {"deepEmerald", {0, 140, 0}},  {"deepYellow", {255, 213, 0}}, {"underwater", {0, 194, 177}}};
    return all;
}

const PhotoFilterPreset* photo_filter(std::string_view id) {
    for (const PhotoFilterPreset& each : photo_filters()) {
        if (each.id == id) return &each;
    }
    return nullptr;
}

double grey_of(const Rgb& c) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; }

std::array<double, 3> rgb_to_hsl(const Rgb& c) {
    const double max = std::max({c[0], c[1], c[2]});
    const double min = std::min({c[0], c[1], c[2]});
    const double l = (max + min) / 2;
    if (std::fabs(max - min) < 1e-7) return {0, 0, l};
    const double d = max - min;
    const double s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
    double h = 0;
    if (max == c[0]) {
        h = wrapped((c[1] - c[2]) / d, 6);
    } else if (max == c[1]) {
        h = (c[2] - c[0]) / d + 2;
    } else {
        h = (c[0] - c[1]) / d + 4;
    }
    return {h / 6, s, l};
}

Rgb hsl_to_rgb(double h, double s, double l) {
    if (s <= 0) return {l, l, l};
    const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
    const double p = 2 * l - q;
    const auto f = [p, q](double t) {
        t = wrapped(t, 1);
        if (t < 1.0 / 6) return p + (q - p) * 6 * t;
        if (t < 0.5) return q;
        if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
        return p;
    };
    return {f(h + 1.0 / 3), f(h), f(h - 1.0 / 3)};
}

double HueRange::weight(double deg) const {
    const double a = bounds[0];
    const auto rel = [a](double x) { return std::isfinite(x) ? wrapped(x - a, 360) : 0.0; };
    const double b = rel(bounds[1]);
    const double c = std::max(rel(bounds[2]), b);
    const double d = std::max(rel(bounds[3]), c);
    const double h = rel(deg);
    if (h <= b) return b <= 0 ? 1.0 : h / b;
    if (h <= c) return 1.0;
    if (h <= d) return d <= c ? 1.0 : (d - h) / (d - c);
    return 0.0;
}

HueRange neutral_range(int i) {
    const double c = 60.0 * i;
    HueRange out;
    out.bounds = {c - 45, c - 15, c + 15, c + 45};
    return out;
}

std::array<double, 4> canonical_bounds(const std::array<double, 4>& b) {
    if (std::any_of(b.begin(), b.end(), [](double v) { return !std::isfinite(v); })) return {-45, -15, 15, 45};
    std::array<double, 4> out{b[0], b[0], b[0], b[0]};
    for (std::size_t k = 1; k < 4; ++k) out[k] = std::min(out[k - 1] + wrapped(b[k] - out[k - 1], 360), out[0] + 359);
    const double centre = (out[1] + out[2]) / 2;
    const double shift = wrapped(centre + 30, 360) - 30 - centre;
    for (double& v : out) v += shift;
    return out;
}

Rgb hue_saturation(const Rgb& c, double hue, double saturation, double lightness, bool colorize) {
    auto [hh, ss, ll] = rgb_to_hsl(c);
    if (colorize) {
        hh = wrapped(hue, 360) / 360;
        ss = std::max(std::fabs(saturation), 0.25);
    } else {
        hh = wrapped(hh + hue / 360, 1);
        ss = std::clamp(ss * (1 + saturation), 0.0, 1.0);
    }
    Rgb out = hsl_to_rgb(hh, ss, ll);
    if (lightness > 0) {
        for (double& v : out) v += (1 - v) * lightness;
    } else if (lightness < 0) {
        for (double& v : out) v *= 1 + lightness;
    }
    return out;
}

double black_white_grey(const Rgb& c, const std::array<double, 6>& weights) {
    const double r = c[0], g = c[1], b = c[2];
    const double max = std::max({r, g, b}), min = std::min({r, g, b});
    const double mid = r + g + b - max - min;
    // primary: the largest channel (red, green, blue); secondary: the two largest together (yellow, cyan, magenta)
    const std::size_t primary = r >= g && r >= b ? 0 : g >= b ? 2 : 4;
    const std::size_t secondary = b <= r && b <= g ? 1 : r <= g ? 3 : 5;
    return std::clamp(min + (mid - min) * weights[secondary] / 100 + (max - mid) * weights[primary] / 100, 0.0, 1.0);
}

std::array<double, 9> selective_color_weights(const Rgb& c) {
    const double max = std::max({c[0], c[1], c[2]});
    const double min = std::min({c[0], c[1], c[2]});
    const double mid = c[0] + c[1] + c[2] - max - min;
    const auto top = [&](std::size_t i) { return c[i] >= max ? max - mid : 0.0; };
    const auto bottom = [&](std::size_t i) { return c[i] <= min ? mid - min : 0.0; };
    return {top(0),
            bottom(2),
            top(1),
            bottom(0),
            top(2),
            bottom(1),
            std::max((min - 0.5) * 2, 0.0),
            std::clamp(1 - std::fabs(max - 0.5) - std::fabs(min - 0.5), 0.0, 1.0),
            std::max((0.5 - max) * 2, 0.0)};
}

Rgb selective_color(const Rgb& c, bool relative, const std::array<std::array<double, 4>, 9>& changes) {
    const std::array<double, 9> w = selective_color_weights(c);
    Rgb delta{0, 0, 0};
    for (std::size_t r = 0; r < 9; ++r) {
        if (w[r] <= 0) continue;
        for (std::size_t i = 0; i < 3; ++i) {
            const double a = changes[r][i] / 100, k = changes[r][3] / 100;
            const double ink = 1 - c[i];
            delta[i] += (relative ? (a + k) * ink : a + k) * w[r];
        }
    }
    return {std::clamp(c[0] - delta[0], 0.0, 1.0), std::clamp(c[1] - delta[1], 0.0, 1.0), std::clamp(c[2] - delta[2], 0.0, 1.0)};
}

}  // namespace color_adjust

namespace {

using color_adjust::Rgb;

double number_at(const Json& s, std::string_view key) { return s.at(std::string(key)).get<double>(); }

template <std::size_t N>
std::array<double, N> numbers_at(const Json& s, std::string_view key) {
    const Json& list = s.at(std::string(key));
    std::array<double, N> out{};
    for (std::size_t i = 0; i < N; ++i) out[i] = list.at(i).get<double>();
    return out;
}

// A whole number as an integer (the stored form's numbers: 15, not 15.0).
Json number_json(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return Json(static_cast<std::int64_t>(v));
    return Json(v);
}

// --- the normal forms (the table read them: clamped, the defaults filled) -------------------------------------------

// Hue/Saturation: the colorize hue is 0..360 (a shift's −180..180, as it was given: an angle below 0 turned once) and
// its saturation 0..100; each range's sliders in their canonical form (PhotoCraft's adjust_params hue_saturation).
Json normal_hue_saturation(Json read, const Page*) {
    const bool colorize = read.value("colorize", false);
    double hue = read.value("hue", 0.0);
    hue = colorize ? (hue < 0 ? hue + 360 : hue) : std::clamp(hue, -180.0, 180.0);
    read["hue"] = number_json(hue);
    if (colorize) read["saturation"] = number_json(std::clamp(read.value("saturation", 0.0), 0.0, 100.0));
    for (const std::string_view key : color_adjust::kHueRanges) {
        const auto range = read.find(std::string(key));
        if (range == read.end() || !range->is_object() || !range->contains("range")) continue;
        const std::array<double, 4> bounds = color_adjust::canonical_bounds(numbers_at<4>(*range, "range"));
        (*range)["range"] = Json::array({number_json(bounds[0]), number_json(bounds[1]), number_json(bounds[2]), number_json(bounds[3])});
    }
    return read;
}

// Photo Filter: a preset names its colour (it is not kept: the colour is the setting; a later change of the colour
// alone is not undone by a preset left behind).
Json normal_photo_filter(Json read, const Page*) {
    const auto it = read.find("filter");
    if (it == read.end()) return read;
    const color_adjust::PhotoFilterPreset* preset = color_adjust::photo_filter(it->get<std::string>());
    if (preset == nullptr) throw OpError("adjust photo_filter.filter is not a preset");  // (the table read its name)
    read["color"] = Json::array({preset->rgb[0], preset->rgb[1], preset->rgb[2]});
    read.erase("filter");
    return read;
}

// The settings of a kind of PC2b's in their normal form, from a correction's adjust (its kind and native set aside; a
// book of another build, or filter_function's caller, may leave some out).
Json settings(const Json& adjust) {
    const std::string kind = adjust.value("kind", std::string());
    const AdjustKind* found = adjust_kind(kind);
    Json given = Json::object();
    for (const auto& [key, value] : adjust.items()) {
        if (key != "kind" && key != "native") given[key] = value;
    }
    Json read = read_params(found->params, given, ReadMode::Op, "adjust " + kind);
    if (kind == "hue_saturation") return normal_hue_saturation(std::move(read), nullptr);
    if (kind == "photo_filter") return normal_photo_filter(std::move(read), nullptr);
    return read;
}

// --- the functions ----------------------------------------------------------------------------------------------------

class HueSaturation final : public AdjustFunction {
public:
    HueSaturation(double hue, double saturation, double lightness, bool colorize, const std::array<color_adjust::HueRange, 6>& ranges)
        : hue_(hue), saturation_(saturation / 100), lightness_(lightness / 100), colorize_(colorize) {
        // (the ranges' changes as three tables over the hue, 4096 steps a turn: PhotoCraft's hue_range_tables; a
        // colorize leaves them out)
        ranged_ = !colorize && std::any_of(ranges.begin(), ranges.end(), [](const color_adjust::HueRange& r) { return !r.neutral(); });
        if (ranged_) {
            for (auto& table : tables_) table.assign(kSteps, 0.0);
            for (std::size_t k = 0; k < kSteps; ++k) {
                const double deg = static_cast<double>(k) / (kSteps - 1) * 360;
                for (const color_adjust::HueRange& r : ranges) {
                    if (r.neutral()) continue;
                    const double w = r.weight(deg);
                    tables_[0][k] += w * r.hue;
                    tables_[1][k] += w * r.saturation / 100;
                    tables_[2][k] += w * r.lightness / 100;
                }
            }
        }
        same_ = !colorize && !ranged_ && hue == 0 && saturation == 0 && lightness == 0;
    }

    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (same_) return c;
        double dh = 0, ds = 0, dl = 0;
        if (ranged_) {
            // (a range follows the colour's own hue, faded out towards grey: a grey has no hue)
            const double h0 = color_adjust::rgb_to_hsl(c)[0];
            const double chroma = std::min((std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]})) * 4, 1.0);
            dh = at(tables_[0], h0) * chroma;
            ds = at(tables_[1], h0) * chroma;
            dl = at(tables_[2], h0) * chroma;
        }
        return color_adjust::hue_saturation(c, hue_ + dh, std::clamp(saturation_ + ds, -1.0, 1.0), std::clamp(lightness_ + dl, -1.0, 1.0),
                                            colorize_);
    }

private:
    static constexpr std::size_t kSteps = 4096;
    static double at(const std::vector<double>& table, double v) {
        const double x = std::clamp(v, 0.0, 1.0) * static_cast<double>(table.size() - 1);
        const auto i = static_cast<std::size_t>(std::floor(x));
        const std::size_t j = std::min(i + 1, table.size() - 1);
        const double f = x - static_cast<double>(i);
        return table[i] * (1 - f) + table[j] * f;
    }
    double hue_, saturation_, lightness_;
    bool colorize_;
    bool ranged_ = false;
    bool same_ = false;
    std::array<std::vector<double>, 3> tables_;
};

// Vibrance: less saturated colours move more (s' = s(1 + S) + V(1 − s)·max(s, 0.1)). A grey — no hue — is left as it is
// (PhotoCraft gives it hue 0: the reds; Photoshop leaves it).
class Vibrance final : public AdjustFunction {
public:
    Vibrance(double vibrance, double saturation) : v_(vibrance / 100), s_(saturation / 100) {}
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (v_ == 0 && s_ == 0) return c;
        const auto [h, sat, l] = color_adjust::rgb_to_hsl(c);
        if (sat == 0) return c;
        const double boost = v_ * (1 - sat);
        const double ns = std::clamp(sat * (1 + s_) + boost * std::max(sat, 0.1), 0.0, 1.0);
        return color_adjust::hsl_to_rgb(h, ns, l);
    }

private:
    double v_, s_;
};

// Color Balance: the shadows' change on the dark (weight 1 − 2l), the highlights' on the light (2l − 1), the midtones'
// on the rest, half a channel at 100; the luminosity kept by scaling to the grey it had.
class ColorBalance final : public AdjustFunction {
public:
    ColorBalance(std::array<std::array<double, 3>, 3> tones, bool preserve) : tones_(tones), preserve_(preserve) {
        same_ = std::all_of(tones_.begin(), tones_.end(), [](const auto& t) { return t[0] == 0 && t[1] == 0 && t[2] == 0; });
    }
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (same_) return c;
        const double l = color_adjust::grey_of(c);
        const double ws = std::clamp(1 - l * 2, 0.0, 1.0);
        const double wh = std::clamp(l * 2 - 1, 0.0, 1.0);
        const double wm = 1 - ws - wh;
        Rgb out{};
        for (std::size_t i = 0; i < 3; ++i)
            out[i] = std::clamp(c[i] + (tones_[0][i] * ws + tones_[1][i] * wm + tones_[2][i] * wh) / 100 * 0.5, 0.0, 1.0);
        if (!preserve_) return out;
        const double l1 = std::max(color_adjust::grey_of(out), 1e-6);
        for (double& v : out) v = std::clamp(v * l / l1, 0.0, 1.0);
        return out;
    }

private:
    std::array<std::array<double, 3>, 3> tones_;
    bool preserve_;
    bool same_ = false;
};

// Black & White: the grey of the weights; a tint laid by (g·t·2 held to 0..1)·½ + g·½.
class BlackWhite final : public AdjustFunction {
public:
    BlackWhite(std::array<double, 6> weights, std::optional<Rgb> tint) : weights_(weights), tint_(tint) {}
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        const double g = color_adjust::black_white_grey(c, weights_);
        if (!tint_) return {g, g, g};
        Rgb out{};
        for (std::size_t i = 0; i < 3; ++i) out[i] = std::clamp(g * (*tint_)[i] * 2, 0.0, 1.0) * 0.5 + g * 0.5;
        return out;
    }

private:
    std::array<double, 6> weights_;
    std::optional<Rgb> tint_;
};

// Photo Filter: the colour multiplied in by the density; the luminosity kept by scaling to the grey it had.
class PhotoFilter final : public AdjustFunction {
public:
    PhotoFilter(Rgb colour, double density, bool preserve) : colour_(colour), density_(density / 100), preserve_(preserve) {}
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (density_ == 0) return c;
        Rgb filtered{};
        for (std::size_t i = 0; i < 3; ++i) filtered[i] = c[i] * (1 - density_) + c[i] * colour_[i] * density_;
        if (!preserve_) return filtered;
        const double l0 = color_adjust::grey_of(c), l1 = std::max(color_adjust::grey_of(filtered), 1e-6);
        for (double& v : filtered) v = std::clamp(v * l0 / l1, 0.0, 1.0);
        return filtered;
    }

private:
    Rgb colour_;
    double density_;
    bool preserve_;
};

// Channel Mixer: each output channel its row · [r, g, b, 1] (percent) held to 0..1; monochrome: the grey row on all three.
class ChannelMixer final : public AdjustFunction {
public:
    ChannelMixer(std::array<std::array<double, 4>, 3> rows, std::array<double, 4> grey, bool monochrome)
        : rows_(rows), grey_(grey), monochrome_(monochrome) {
        same_ = !monochrome && rows_ == std::array<std::array<double, 4>, 3>{{{100, 0, 0, 0}, {0, 100, 0, 0}, {0, 0, 100, 0}}};
    }
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (same_) return c;
        const auto mix = [&c](const std::array<double, 4>& row) {
            return std::clamp((row[0] * c[0] + row[1] * c[1] + row[2] * c[2] + row[3]) / 100, 0.0, 1.0);
        };
        if (monochrome_) {
            const double g = mix(grey_);
            return {g, g, g};
        }
        return {mix(rows_[0]), mix(rows_[1]), mix(rows_[2])};
    }

private:
    std::array<std::array<double, 4>, 3> rows_;
    std::array<double, 4> grey_;
    bool monochrome_;
    bool same_ = false;
};

class SelectiveColor final : public AdjustFunction {
public:
    SelectiveColor(bool relative, std::array<std::array<double, 4>, 9> changes) : relative_(relative), changes_(changes) {
        same_ = std::all_of(changes_.begin(), changes_.end(), [](const auto& r) { return r == std::array<double, 4>{0, 0, 0, 0}; });
    }
    std::array<double, 3> apply(const std::array<double, 3>& c, int, int) const override {
        if (same_) return c;
        return color_adjust::selective_color(c, relative_, changes_);
    }

private:
    bool relative_;
    std::array<std::array<double, 4>, 9> changes_;
    bool same_ = false;
};

AdjustPtr function_of(const Json& adjust, const Page&, const Document*) {
    const std::string kind = adjust.value("kind", std::string());
    const Json s = settings(adjust);
    if (kind == "hue_saturation") {
        std::array<color_adjust::HueRange, 6> ranges{};
        for (std::size_t i = 0; i < 6; ++i) {
            const Json& r = s.at(std::string(color_adjust::kHueRanges[i]));
            ranges[i].hue = number_at(r, "hue");
            ranges[i].saturation = number_at(r, "saturation");
            ranges[i].lightness = number_at(r, "lightness");
            ranges[i].bounds = numbers_at<4>(r, "range");
        }
        return std::make_shared<HueSaturation>(number_at(s, "hue"), number_at(s, "saturation"), number_at(s, "lightness"), s.at("colorize").get<bool>(),
                                               ranges);
    }
    if (kind == "desaturate") {  // (Hue/Saturation's saturation −100: each colour its HSL lightness)
        std::array<color_adjust::HueRange, 6> ranges{};
        for (int i = 0; i < 6; ++i) ranges[static_cast<std::size_t>(i)] = color_adjust::neutral_range(i);
        return std::make_shared<HueSaturation>(0, -100, 0, false, ranges);
    }
    if (kind == "vibrance") return std::make_shared<Vibrance>(number_at(s, "vibrance"), number_at(s, "saturation"));
    if (kind == "color_balance") {
        return std::make_shared<ColorBalance>(std::array<std::array<double, 3>, 3>{numbers_at<3>(s, "shadows"), numbers_at<3>(s, "midtones"),
                                                                                   numbers_at<3>(s, "highlights")},
                                              s.at("preserve_luminosity").get<bool>());
    }
    if (kind == "black_white") {
        std::array<double, 6> weights{};
        for (std::size_t i = 0; i < 6; ++i) weights[i] = number_at(s, color_adjust::kHueRanges[i]);
        std::optional<Rgb> tint;
        if (s.at("tint").get<bool>()) {
            const auto t = numbers_at<3>(s, "tint_color");
            tint = Rgb{t[0] / 255, t[1] / 255, t[2] / 255};
        }
        return std::make_shared<BlackWhite>(weights, tint);
    }
    if (kind == "photo_filter") {
        const auto colour = numbers_at<3>(s, "color");
        return std::make_shared<PhotoFilter>(Rgb{colour[0] / 255, colour[1] / 255, colour[2] / 255}, number_at(s, "density"),
                                             s.at("preserve_luminosity").get<bool>());
    }
    if (kind == "channel_mixer") {
        return std::make_shared<ChannelMixer>(std::array<std::array<double, 4>, 3>{numbers_at<4>(s, "red"), numbers_at<4>(s, "green"), numbers_at<4>(s, "blue")},
                                              numbers_at<4>(s, "gray"), s.at("monochrome").get<bool>());
    }
    if (kind == "selective_color") {
        std::array<std::array<double, 4>, 9> changes{};
        for (std::size_t i = 0; i < 9; ++i) changes[i] = numbers_at<4>(s, color_adjust::kSelectiveRanges[i]);
        return std::make_shared<SelectiveColor>(s.at("method") == Json("relative"), changes);
    }
    return nullptr;
}

}  // namespace

HookTable color_hooks() {
    HookTable out;
    for (const std::string_view kind :
         {"hue_saturation", "vibrance", "color_balance", "black_white", "photo_filter", "channel_mixer", "selective_color", "desaturate"}) {
        AdjustHooks hooks;
        if (kind == "hue_saturation") hooks.normalize = normal_hue_saturation;
        if (kind == "photo_filter") hooks.normalize = normal_photo_filter;
        hooks.function = function_of;
        out.emplace_back(kind, std::move(hooks));
    }
    return out;
}

}  // namespace genko::core
