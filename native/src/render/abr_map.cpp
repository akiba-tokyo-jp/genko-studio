#include "render/abr_map.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>

#include "core/brush_dabs.hpp"
#include "core/error.hpp"
#include "render/dabs/tile.hpp"
#include "storage/asset_store.hpp"

namespace genko::render::abr_map {

namespace {

using core::Json;
using Object = psd::descriptor::Object;
using Value = psd::descriptor::Value;

// --- the descriptors' values ---------------------------------------------------------------------------------------

std::optional<double> num(const Object& d, std::string_view k) {
    const Value* v = d.get(k);
    if (v == nullptr) return std::nullopt;
    const std::optional<double> n = v->numeric();
    if (!n || !std::isfinite(*n)) return std::nullopt;
    return n;
}
// a percentage as 0..1
std::optional<double> pct(const Object& d, std::string_view k) {
    const auto v = num(d, k);
    return v ? std::optional<double>(*v / 100.0) : std::nullopt;
}
std::optional<bool> boolean(const Object& d, std::string_view k) {
    const Value* v = d.get(k);
    if (v == nullptr || v->type != "bool") return std::nullopt;
    return v->boolean;
}
std::optional<std::string> text(const Object& d, std::string_view k) {
    const Value* v = d.get(k);
    if (v == nullptr || v->type != "TEXT") return std::nullopt;
    return psd::utf8_of(v->text);
}
const Object* obj(const Object& d, std::string_view k) {
    const Value* v = d.get(k);
    if (v == nullptr || (v->type != "Objc" && v->type != "GlbO") || !v->object) return nullptr;
    return v->object.get();
}
std::optional<std::string> enum_value(const Object& d, std::string_view k) {
    const Value* v = d.get(k);
    if (v == nullptr || v->type != "enum") return std::nullopt;
    return v->enum_value;
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::optional<core::DabMaskMode> mask_mode(const std::optional<std::string>& code) {
    if (!code) return std::nullopt;
    using M = core::DabMaskMode;
    static const std::map<std::string, M> modes{{"Mltp", M::Multiply},       {"Sbtr", M::Subtract},        {"Drkn", M::Darken},
                                                {"Ovrl", M::Overlay},        {"CDdg", M::ColorDodge},      {"CBrn", M::ColorBurn},
                                                {"linearBurn", M::LinearBurn}, {"hardMix", M::HardMix},    {"linearHeight", M::LinearHeight},
                                                {"height", M::Height},       {"Hght", M::Height}};
    const auto it = modes.find(*code);
    return it == modes.end() ? std::nullopt : std::optional<M>(it->second);
}

// The preset's keys this reads (any other is named in the warnings).
const std::set<std::string>& known() {
    static const std::set<std::string> keys{
        "Nm  ",          "Brsh",           "useTipDynamics",    "flipX",           "flipY",               "brushProjection",
        "minimumDiameter", "minimumRoundness", "tiltScale",     "szVr",            "angleDynamics",       "roundnessDynamics",
        "useScatter",    "Spcn",           "Cnt ",              "bothAxes",        "countDynamics",       "scatterDynamics",
        "dualBrush",     "brushGroup",     "useTexture",        "TxtC",            "interpretation",      "textureBlendMode",
        "textureDepth",  "minimumDepth",   "textureDepthDynamics", "Txtr",         "textureScale",        "InvT",
        "protectTexture", "textureBrightness", "textureContrast", "usePaintDynamics", "prVr",              "opVr",
        "wtVr",          "mxVr",           "useColorDynamics",  "clVr",            "H   ",                "Strt",
        "Brgh",          "purity",         "colorDynamicsPerTip", "Wtdg",          "Nose",                "Rpt ",
        "useLegacy",     "useBrushPose",   "overridePoseAngle", "overridePoseTiltX", "overridePoseTiltY", "overridePosePressure",
        "brushPosePressure", "brushPoseTiltX", "brushPoseTiltY", "brushPoseAngle",  "toolOptions"};
    return keys;
}

// --- pictures ------------------------------------------------------------------------------------------------------

// A plane of `w` × `h` (each value from `value`) made no larger than `most` a side by averaging boxes (upstream's fit).
struct Plane {
    std::uint32_t width = 0, height = 0;
    std::vector<float> values;
};
Plane fit(std::uint32_t w, std::uint32_t h, const std::function<float(std::size_t)>& value, std::uint32_t most) {
    Plane out;
    if (w <= most && h <= most) {
        out.width = w;
        out.height = h;
        out.values.resize(std::size_t(w) * h);
        for (std::size_t i = 0; i < out.values.size(); ++i) out.values[i] = value(i);
        return out;
    }
    const float k = std::max(1.0f, static_cast<float>(std::max(w, h)) / static_cast<float>(most));
    out.width = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(static_cast<float>(w) / k)), 1, most);
    out.height = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(static_cast<float>(h) / k)), 1, most);
    out.values.assign(std::size_t(out.width) * out.height, 0.0f);
    const std::uint32_t nw = out.width, nh = out.height;
    for (std::uint32_t y = 0; y < nh; ++y) {
        const std::uint32_t y0 = static_cast<std::uint32_t>(std::uint64_t(y) * h / nh);
        const std::uint32_t y1 = std::min<std::uint32_t>(std::max<std::uint32_t>(static_cast<std::uint32_t>(std::uint64_t(y + 1) * h / nh), y0 + 1), h);
        for (std::uint32_t x = 0; x < nw; ++x) {
            const std::uint32_t x0 = static_cast<std::uint32_t>(std::uint64_t(x) * w / nw);
            const std::uint32_t x1 = std::min<std::uint32_t>(std::max<std::uint32_t>(static_cast<std::uint32_t>(std::uint64_t(x + 1) * w / nw), x0 + 1), w);
            double sum = 0.0;
            std::size_t n = 0;
            for (std::uint32_t yy = y0; yy < y1; ++yy) {
                for (std::uint32_t xx = x0; xx < x1; ++xx) {
                    sum += value(std::size_t(yy) * w + xx);
                    ++n;
                }
            }
            out.values[std::size_t(y) * nw + x] = n > 0 ? static_cast<float>(sum / static_cast<double>(n)) : 0.0f;
        }
    }
    return out;
}

// A plane kept as a grey picture: its ref (the picture in `pictures`).
std::string keep(const Plane& plane, std::map<std::string, core::Bytes>& pictures) {
    dabs::GrayTile tile;
    tile.width = plane.width;
    tile.height = plane.height;
    tile.data.resize(plane.values.size());
    for (std::size_t i = 0; i < plane.values.size(); ++i) tile.data[i] = dabs::q16(std::clamp(plane.values[i], 0.0f, 1.0f));
    auto png = std::make_shared<const std::string>(dabs::png_of_tile(tile));
    std::string ref = storage::AssetStore::ref(*png);
    pictures.emplace(ref, std::move(png));
    return ref;
}

// A pattern of the file as a texture: its lightness (transparent parts white: upstream reads the planes only), none
// for one that cannot be.
std::optional<Plane> pattern_plane(const pat::PsdPattern& p) {
    const std::size_t n = std::size_t(p.width) * p.height;
    if (n == 0) return std::nullopt;
    const auto plane = [&](std::size_t c) -> std::optional<std::vector<float>> {
        if (c >= p.channels.size()) return std::nullopt;
        const std::string& ch = p.channels[c];
        std::vector<float> v;
        if (p.depth == 8 && ch.size() >= n) {
            v.resize(n);
            for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<float>(static_cast<std::uint8_t>(ch[i])) / 255.0f;
        } else if (p.depth == 16 && ch.size() >= 2 * n) {
            v.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                v[i] = static_cast<float>((static_cast<std::uint8_t>(ch[2 * i]) << 8) | static_cast<std::uint8_t>(ch[2 * i + 1])) / 65535.0f;
            }
        } else if (p.depth == 32 && ch.size() >= 4 * n) {
            v.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t bits = (std::uint32_t(static_cast<std::uint8_t>(ch[4 * i])) << 24) | (std::uint32_t(static_cast<std::uint8_t>(ch[4 * i + 1])) << 16) |
                                           (std::uint32_t(static_cast<std::uint8_t>(ch[4 * i + 2])) << 8) | std::uint32_t(static_cast<std::uint8_t>(ch[4 * i + 3]));
                float f = 0.0f;
                std::memcpy(&f, &bits, sizeof f);
                v[i] = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
            }
        } else {
            return std::nullopt;
        }
        return v;
    };
    std::vector<float> grey;
    if (p.mode == 3) {
        const auto r = plane(0), g = plane(1), b = plane(2);
        if (!r || !g || !b) return std::nullopt;
        grey.resize(n);
        for (std::size_t i = 0; i < n; ++i) grey[i] = 0.299f * (*r)[i] + 0.587f * (*g)[i] + 0.114f * (*b)[i];
    } else if (p.mode == 2) {
        if (p.palette.size() < 768 || p.channels.empty() || p.channels[0].size() < n) return std::nullopt;
        grey.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t k = std::size_t(static_cast<std::uint8_t>(p.channels[0][i])) * 3;
            const auto c = [&](std::size_t o) { return static_cast<float>(static_cast<std::uint8_t>(p.palette[k + o])) / 255.0f; };
            grey[i] = 0.299f * c(0) + 0.587f * c(1) + 0.114f * c(2);
        }
    } else if (p.mode == 4) {  // (CMYK planes are kept inverted: 0 is full ink)
        const auto c = plane(0), k = plane(3);
        if (!c || !k) return std::nullopt;
        grey.resize(n);
        for (std::size_t i = 0; i < n; ++i) grey[i] = std::clamp((*c)[i] * (*k)[i], 0.0f, 1.0f);
    } else {
        auto first = plane(0);
        if (!first) return std::nullopt;
        grey = std::move(*first);
    }
    return fit(p.width, p.height, [&grey](std::size_t i) { return grey[i]; }, core::kMaxPatternEdge);
}

struct Mapper {
    const abr::File& file;
    Import& out;
    std::set<std::string> warnings;
    std::set<std::string> unknown;
    std::map<const abr::Sample*, std::pair<std::string, double>> tips;  // (each tip kept once)

    // A sampled tip (made no larger than 2500 a side) and its size (its larger side, pixels).
    std::pair<std::string, double> tip(const abr::Sample& s) {
        if (const auto it = tips.find(&s); it != tips.end()) return it->second;
        const Plane plane = fit(s.width, s.height, [&s](std::size_t i) { return s.unit(i); }, core::kMaxTipEdge);
        if (plane.width != s.width || plane.height != s.height) {
            warnings.insert("一辺 " + std::to_string(core::kMaxTipEdge) + " 画素より大きい先端は小さくしました");
        }
        auto made = std::pair<std::string, double>{keep(plane, out.pictures), static_cast<double>(std::max(s.width, s.height))};
        tips.emplace(&s, made);
        return made;
    }

    core::DabDynamic dynamic(const Object* d) {
        core::DabDynamic out_dyn;
        if (d == nullptr) return out_dyn;
        using C = core::DabControl;
        switch (static_cast<long long>(num(*d, "bVTy").value_or(0.0))) {
            case 1: out_dyn.control = C::Fade; break;
            case 2: out_dyn.control = C::Pressure; break;
            case 3: out_dyn.control = C::Tilt; break;
            case 4: out_dyn.control = C::Wheel; break;
            case 5: out_dyn.control = C::InitialDirection; break;
            case 6: out_dyn.control = C::Direction; break;
            case 7:
                warnings.insert("コントロールの「初期回転」は「回転」にしました");
                out_dyn.control = C::Rotation;
                break;
            case 8: out_dyn.control = C::Rotation; break;
            default: out_dyn.control = C::Off; break;
        }
        out_dyn.jitter = std::max(0.0, pct(*d, "jitter").value_or(0.0));
        out_dyn.fade_steps = num(*d, "fStp") ? static_cast<std::int64_t>(std::clamp(*num(*d, "fStp"), 1.0, 9999.0)) : 25;
        out_dyn.minimum = std::clamp(pct(*d, "Mnm ").value_or(0.0), 0.0, 1.0);
        return out_dyn;
    }
    static core::DabDynamic capped(core::DabDynamic d, double most) {
        d.jitter = std::clamp(d.jitter, 0.0, most);
        return d;
    }

    // A brush's size in pixels as mm (0.01..500), saying when it is past what a brush holds.
    double mm(double px) {
        const double v = px * kMmPerPixel;
        if (v > 500.0) warnings.insert("500 mm より大きいブラシは 500 mm にしました");
        return std::clamp(v, 0.01, 500.0);
    }

    // The tip of a computedBrush / sampledBrush object: its ref ("" round) and the sampled size; none when the sampled
    // tip it names is not in the file.
    std::optional<std::pair<std::string, std::optional<double>>> tip_shape(const Object& t) {
        const bool sampled = t.class_id == "sampledBrush" || t.get("sampledData") != nullptr;
        if (!sampled) return std::pair<std::string, std::optional<double>>{std::string(), std::nullopt};
        const std::string id = text(t, "sampledData").value_or(std::string());
        const abr::Sample* found = nullptr;
        for (const abr::Sample& s : file.samples) {
            if (s.id == id) {
                found = &s;
                break;
            }
        }
        if (found == nullptr && file.samples.size() == 1) found = &file.samples.front();
        if (found == nullptr) return std::nullopt;
        const auto [ref, size] = tip(*found);
        return std::pair<std::string, std::optional<double>>{ref, size};
    }

    core::DabPattern pattern(const Object& t) {
        const std::string id = text(t, "Idnt").value_or(std::string());
        const std::string name = text(t, "Nm  ").value_or(std::string());
        const pat::PsdPattern* found = nullptr;
        for (const pat::PsdPattern& p : file.patterns) {
            if (!id.empty() && p.id == id) {
                found = &p;
                break;
            }
        }
        if (found == nullptr) {
            for (const pat::PsdPattern& p : file.patterns) {
                if (!name.empty() && p.name == name) {
                    found = &p;
                    break;
                }
            }
        }
        core::DabPattern out_pattern;
        if (found != nullptr) {
            if (const auto plane = pattern_plane(*found)) {
                out_pattern.asset = keep(*plane, out.pictures);
                return out_pattern;
            }
        }
        warnings.insert("テクスチャのパターン「" + pat::shown_name(name.empty() ? id : name) + "」はファイルに入っていないので、紙の模様で代えました");
        return out_pattern;  // (the default: the program's paper, 128 pixels, seed 1)
    }

    std::optional<core::DabSettings> preset(const Object& d) {
        for (const auto& [k, v] : d.items) {
            if (!known().contains(k)) unknown.insert(k);
        }
        core::DabSettings b;
        b.pressure_size = false;
        b.spacing = 0.25;
        double size_px = 20.0;
        if (const Object* t = obj(d, "Brsh")) {
            const auto shape = tip_shape(*t);
            if (!shape) return std::nullopt;
            b.tip = shape->first;
            size_px = std::clamp(num(*t, "Dmtr").value_or(shape->second.value_or(size_px)), 1.0, 5000.0);
            if (const auto h = pct(*t, "Hrdn")) b.hardness = std::clamp(*h, 0.0, 1.0);
            b.angle = std::fmod(num(*t, "Angl").value_or(0.0), 360.0);
            b.roundness = std::clamp(pct(*t, "Rndn").value_or(1.0), 0.01, 1.0);
            // (間隔 off: Photoshop spaces the dabs by the pointer's speed — kept close)
            b.spacing = boolean(*t, "Intr") == std::optional<bool>(false) ? 0.05 : std::clamp(pct(*t, "Spcn").value_or(0.25), 0.01, 10.0);
            b.flip_x = boolean(*t, "flipX").value_or(false);
            b.flip_y = boolean(*t, "flipY").value_or(false);
        }
        b.size_mm = mm(size_px);
        if (boolean(d, "useTipDynamics") == std::optional<bool>(true)) {
            core::DabDynamic size = capped(dynamic(obj(d, "szVr")), 1.0);
            size.minimum = std::clamp(pct(d, "minimumDiameter").value_or(size.minimum), 0.0, 1.0);
            core::DabDynamic roundness = capped(dynamic(obj(d, "roundnessDynamics")), 1.0);
            roundness.minimum = std::clamp(pct(d, "minimumRoundness").value_or(roundness.minimum), 0.0, 1.0);
            b.shape.enabled = true;
            b.shape.size = size;
            b.shape.angle = capped(dynamic(obj(d, "angleDynamics")), 1.0);
            b.shape.roundness = roundness;
            b.shape.flip_x_jitter = boolean(d, "flipX").value_or(false);
            b.shape.flip_y_jitter = boolean(d, "flipY").value_or(false);
            if (pct(d, "tiltScale").value_or(0.0) > 0.0) warnings.insert("シェイプの「傾きの比率」は使えません");
            if (boolean(d, "brushProjection") == std::optional<bool>(true)) warnings.insert("「ブラシ投影」は使えません");
        }
        if (boolean(d, "useScatter") == std::optional<bool>(true)) {
            b.scatter.enabled = true;
            b.scatter.scatter = capped(dynamic(obj(d, "scatterDynamics")), 10.0);
            b.scatter.both_axes = boolean(d, "bothAxes").value_or(false);
            b.scatter.count = num(d, "Cnt ") ? static_cast<std::int64_t>(std::clamp(*num(d, "Cnt "), 1.0, 16.0)) : 1;
            b.scatter.count_jitter = capped(dynamic(obj(d, "countDynamics")), 1.0);
        }
        if (boolean(d, "useTexture") == std::optional<bool>(true)) {
            core::DabTexture& t = b.texture;
            t.enabled = true;
            t.each_tip = boolean(d, "TxtC").value_or(false);
            t.invert = boolean(d, "InvT").value_or(false);
            t.scale = std::clamp(pct(d, "textureScale").value_or(1.0), 0.01, 10.0);
            t.brightness = std::clamp(num(d, "textureBrightness").value_or(0.0) / 150.0, -1.0, 1.0);
            const double contrast = num(d, "textureContrast").value_or(0.0) / 50.0;
            if (contrast > 1.0) warnings.insert("テクスチャのコントラストが 50 を越えるものは 50 にしました");
            t.contrast = std::clamp(contrast, -1.0, 1.0);
            t.mode = mask_mode(enum_value(d, "textureBlendMode")).value_or(core::DabMaskMode::Multiply);
            t.depth = std::clamp(pct(d, "textureDepth").value_or(1.0), 0.0, 1.0);
            core::DabDynamic depth = capped(dynamic(obj(d, "textureDepthDynamics")), 1.0);
            depth.minimum = std::clamp(pct(d, "minimumDepth").value_or(depth.minimum), 0.0, 1.0);
            t.depth_jitter = depth;
            if (const Object* p = obj(d, "Txtr")) t.pattern = pattern(*p);
        }
        b.protect_texture = boolean(d, "protectTexture").value_or(false);
        if (const Object* db = obj(d, "dualBrush"); db != nullptr && boolean(*db, "useDualBrush") == std::optional<bool>(true)) {
            core::DabDual dual;
            dual.enabled = true;
            double dual_px = 20.0;
            if (const Object* t = obj(*db, "Brsh")) {
                if (const auto shape = tip_shape(*t)) {
                    dual.tip = shape->first;
                    dual_px = std::clamp(num(*t, "Dmtr").value_or(shape->second.value_or(dual_px)), 1.0, 5000.0);
                } else {
                    warnings.insert("デュアルブラシの先端がファイルに入っていないので、円の先端で代えました");
                }
                dual.hardness = std::clamp(pct(*t, "Hrdn").value_or(1.0), 0.0, 1.0);
                dual.roundness = std::clamp(pct(*t, "Rndn").value_or(1.0), 0.01, 1.0);
                dual.angle = std::fmod(num(*t, "Angl").value_or(0.0), 360.0);
                dual.spacing = std::clamp(pct(*t, "Spcn").value_or(dual.spacing), 0.01, 10.0);
            }
            dual.size_mm = mm(dual_px);
            dual.mode = mask_mode(enum_value(*db, "BlnM")).value_or(core::DabMaskMode::Multiply);
            dual.flip = boolean(*db, "Flip").value_or(false);
            if (const auto s = pct(*db, "Spcn")) dual.spacing = std::clamp(*s, 0.01, 10.0);
            dual.scatter = std::clamp(dynamic(obj(*db, "scatterDynamics")).jitter, 0.0, 10.0);
            dual.both_axes = boolean(*db, "bothAxes").value_or(false);
            dual.count = num(*db, "Cnt ") ? static_cast<std::int64_t>(std::clamp(*num(*db, "Cnt "), 1.0, 16.0)) : 1;
            b.dual = dual;
        }
        if (boolean(d, "useColorDynamics") == std::optional<bool>(true)) {
            core::DabColor& c = b.color;
            c.enabled = true;
            c.per_tip = boolean(d, "colorDynamicsPerTip").value_or(true);
            c.hue_jitter = std::clamp(pct(d, "H   ").value_or(0.0), 0.0, 1.0);
            c.saturation_jitter = std::clamp(pct(d, "Strt").value_or(0.0), 0.0, 1.0);
            c.brightness_jitter = std::clamp(pct(d, "Brgh").value_or(0.0), 0.0, 1.0);
            c.purity = std::clamp(pct(d, "purity").value_or(0.0), -1.0, 1.0);
            c.fg_bg = capped(dynamic(obj(d, "clVr")), 1.0);
        }
        if (boolean(d, "usePaintDynamics") == std::optional<bool>(true)) {
            b.transfer.enabled = true;
            b.transfer.opacity = capped(dynamic(obj(d, "opVr")), 1.0);
            b.transfer.flow = capped(dynamic(obj(d, "prVr")), 1.0);
            for (const auto& [k, what] : {std::pair{"wtVr", "ウェット"}, std::pair{"mxVr", "ミックス"}}) {
                const core::DabDynamic v = dynamic(obj(d, k));
                if (v.jitter > 0.0 || v.control != core::DabControl::Off) {
                    warnings.insert(std::string("伝達の「") + what + "」の揺らぎ（混合ブラシ）は使えません");
                }
            }
        }
        if (boolean(d, "useBrushPose") == std::optional<bool>(true)) {  // (Photoshop's tilt is ±100 %; here ±90°)
            core::DabPose& p = b.pose;
            p.enabled = true;
            p.tilt_x = std::clamp(num(d, "brushPoseTiltX").value_or(0.0) * 0.9, -90.0, 90.0);
            p.tilt_y = std::clamp(num(d, "brushPoseTiltY").value_or(0.0) * 0.9, -90.0, 90.0);
            const double turn = std::fmod(num(d, "brushPoseAngle").value_or(0.0), 360.0);
            p.rotation = turn < 0.0 ? turn + 360.0 : turn;
            if (p.rotation >= 360.0) p.rotation = 0.0;
            p.pressure = std::clamp(pct(d, "brushPosePressure").value_or(1.0), 0.0, 1.0);
            p.override_tilt = boolean(d, "overridePoseTiltX").value_or(false) || boolean(d, "overridePoseTiltY").value_or(false);
            p.override_rotation = boolean(d, "overridePoseAngle").value_or(false);
            p.override_pressure = boolean(d, "overridePosePressure").value_or(false);
        }
        b.noise = boolean(d, "Nose").value_or(false);
        b.wet_edges = boolean(d, "Wtdg").value_or(false);
        b.build_up = boolean(d, "Rpt ").value_or(false);
        if (const Object* to = obj(d, "toolOptions")) {  // (「ツールの設定を含める」)
            if (const auto v = pct(*to, "Opct")) b.opacity = std::clamp(*v, 0.0, 1.0);
            if (const auto v = pct(*to, "flow")) b.flow = std::clamp(*v, 0.0, 1.0);
            b.pressure_size = boolean(*to, "usePressureOverridesSize").value_or(false);
            b.pressure_opacity = boolean(*to, "usePressureOverridesOpacity").value_or(false);
            if (boolean(*to, "smoothing").value_or(false) && pct(*to, "smoothingValue").value_or(0.0) > 0.0) {
                warnings.insert("プリセットの「滑らかさ」は使いません（ツールの設定の滑らかさで描きます）");
            }
            if (const auto mode = enum_value(*to, "Md  "); mode && *mode != "Nrml") {
                warnings.insert("プリセットに保存された描画モードは使いません（ブラシ パネルのモードで描きます）");
            }
        }
        return b;
    }
};

Json settings_of(const std::string& name, const core::DabSettings& dabs) {
    // (checked as a book checks them: what is kept is what a book takes)
    const Json checked = core::dabs_to_json(core::dabs_from_json(core::dabs_to_json(dabs)));
    return Json{{"label", name}, {"width_mm", std::clamp(dabs.size_mm, 0.05, 50.0)}, {"engine", std::string(core::kDabEngine)}, {"dabs", checked}};
}

}  // namespace

Import map_file(const abr::File& f) {
    Import out;
    out.version = f.version;
    out.warnings = f.warnings;
    Mapper m{f, out, {}, {}, {}};
    if (!f.legacy.empty()) {
        for (std::size_t i = 0; i < f.legacy.size(); ++i) {
            const abr::LegacyBrush& b = f.legacy[i];
            core::DabSettings s;
            s.pressure_size = false;
            s.spacing = b.spacing == 0 ? 0.25 : std::clamp(b.spacing / 100.0, 0.01, 10.0);
            std::string fallback;
            if (b.computed) {
                s.size_mm = m.mm(std::max<double>(b.diameter, 1.0));
                s.hardness = std::clamp(b.hardness / 100.0, 0.0, 1.0);
                s.angle = std::fmod(static_cast<double>(b.angle), 360.0);
                s.roundness = std::clamp(b.roundness / 100.0, 0.01, 1.0);
                fallback = "円ブラシ " + std::to_string(b.diameter) + " px";
            } else {
                const auto [ref, size] = m.tip(b.sample);
                s.tip = ref;
                s.size_mm = m.mm(size);
                s.aliased = !b.anti_alias;
                fallback = "サンプルブラシ " + std::to_string(i + 1);
            }
            const std::string name = trimmed(b.name);
            out.presets.push_back(Preset{name.empty() ? fallback : name, settings_of(name.empty() ? fallback : name, s)});
        }
    } else if (f.presets.empty()) {  // (tips without settings: one brush each)
        for (std::size_t i = 0; i < f.samples.size(); ++i) {
            core::DabSettings s;
            s.pressure_size = false;
            s.spacing = 0.25;
            const auto [ref, size] = m.tip(f.samples[i]);
            s.tip = ref;
            s.size_mm = m.mm(size);
            const std::string name = "サンプルブラシ " + std::to_string(i + 1);
            out.presets.push_back(Preset{name, settings_of(name, s)});
        }
    } else {
        for (std::size_t i = 0; i < f.presets.size(); ++i) {
            const Object& d = f.presets[i];
            std::string name = trimmed(text(d, "Nm  ").value_or(std::string()));
            if (name.empty()) name = "ブラシ " + std::to_string(i + 1);
            if (const auto s = m.preset(d)) {
                out.presets.push_back(Preset{name, settings_of(name, *s)});
            } else {
                m.warnings.insert("「" + name + "」はサンプルの先端がファイルに入っていないので読み込みませんでした");
            }
        }
    }
    for (const std::string& w : m.warnings) out.warnings.push_back(w);
    if (!m.unknown.empty()) {
        std::string list;
        for (const std::string& k : m.unknown) list += (list.empty() ? "" : ", ") + k;
        out.warnings.push_back("Genko に当たる物の無い設定は使いませんでした: " + list);
    }
    // (only the pictures the presets kept name)
    std::set<std::string> named;
    for (const Preset& p : out.presets) {
        for (const std::string& ref : core::dab_assets(core::dabs_from_json(p.settings["dabs"]))) named.insert(ref);
    }
    std::erase_if(out.pictures, [&named](const auto& entry) { return !named.contains(entry.first); });
    return out;
}

Import read(std::string_view bytes) { return map_file(abr::parse(bytes)); }

}  // namespace genko::render::abr_map
