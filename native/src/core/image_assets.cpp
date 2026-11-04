// image-assets@1 (PC5 I-f): the book's generator setting, its op, and the layers' naming rule (PhotoCraft v0.2.0's
// engine/src/web_cmds.rs parse_size, parse_asset_name and parse_defaults, MIT).

#include "core/image_assets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>

#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyops.hpp"

namespace genko::core {

Json image_assets_json() { return Json{{"image_assets", true}}; }

bool image_assets_from_json(const Json& value) {
    // ({"image_assets": false} — another program's way of saying off — is read as off; written back as none)
    if (!value.is_object() || value.size() != 1 || !value.contains("image_assets") || !value["image_assets"].is_boolean()) {
        throw Error("value", "the book's generator is {\"image_assets\": true} or none");
    }
    return value["image_assets"].get<bool>();
}

void register_image_asset_ops(OpRegistry& registry) {
    registry.add("set_image_assets", [](OpContext& c) {
        const Json* on = get(c.op, "on");
        if (on == nullptr || !on->is_boolean()) throw OpError("on is true or false (生成 → 画像アセット)");
        c.doc.image_assets = on->get<bool>();
        c.report = Json{{"image_assets", c.doc.image_assets}};
    });
}

namespace image_assets {

namespace {

std::string_view trim(std::string_view s) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A whole decimal number (Rust's str::parse::<f64> as the rule takes it: finite only here).
std::optional<double> number(std::string_view text) {
    text = trim(text);
    if (text.empty() || text.size() > 64) return std::nullopt;
    const std::string s(text);
    for (const char c : s) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')) return std::nullopt;
    }
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || errno != 0 || !std::isfinite(v)) return std::nullopt;
    return v;
}

// Rust's str::parse::<u32>: digits only.
std::optional<long long> whole(std::string_view text) {
    if (text.empty() || text.size() > 9) return std::nullopt;
    long long v = 0;
    for (const char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

// unit_px: "?" (in proportion), or a positive number with px, in, cm or mm.
std::optional<std::optional<double>> unit_px(std::string_view token, double dpi) {
    if (token == "?") return std::optional<double>();
    double k = 1.0;
    std::string_view n = token;
    if (n.ends_with("px")) {
        n.remove_suffix(2);
    } else if (n.ends_with("in")) {
        n.remove_suffix(2);
        k = dpi;
    } else if (n.ends_with("cm")) {
        n.remove_suffix(2);
        k = dpi / 2.54;
    } else if (n.ends_with("mm")) {
        n.remove_suffix(2);
        k = dpi / 25.4;
    }
    const auto v = number(n);
    if (!v || !(*v > 0.0)) return std::nullopt;
    return std::optional<double>(*v * k);
}

struct Size {
    std::optional<double> scale, width, height;
    std::string_view rest;
};

// parse_size: "200%" or "WxH" (px, in, cm, mm; "?") at the start of the item — or the spaced "2in x 1in" —; the rest.
Size parse_size(std::string_view item, double dpi) {
    item = trim(item);
    std::string_view first = item, rest;
    if (const auto at = std::find_if(item.begin(), item.end(), is_space); at != item.end()) {
        first = item.substr(0, static_cast<std::size_t>(at - item.begin()));
        rest = trim(item.substr(first.size()));
    }
    if (first.ends_with('%')) {
        if (const auto pc = number(first.substr(0, first.size() - 1)); pc && *pc > 0.0) return Size{*pc / 100.0, std::nullopt, std::nullopt, rest};
    }
    if (const auto x = first.find_first_of("xX"); x != std::string_view::npos && x > 0 && x + 1 < first.size()) {
        const auto w = unit_px(first.substr(0, x), dpi), h = unit_px(first.substr(x + 1), dpi);
        if (w && h) return Size{std::nullopt, *w, *h, rest};
    }
    // (the spaced form "2in x 1in name.png": three tokens before the name)
    std::vector<std::string_view> tokens;
    for (std::size_t i = 0; i < item.size();) {
        while (i < item.size() && is_space(item[i])) ++i;
        std::size_t j = i;
        while (j < item.size() && !is_space(item[j])) ++j;
        if (j > i) tokens.push_back(item.substr(i, j - i));
        i = j;
    }
    if (tokens.size() >= 4 && lower(tokens[1]) == "x") {
        const auto w = unit_px(tokens[0], dpi), h = unit_px(tokens[2], dpi);
        if (w && h) {
            const std::size_t at = item.find(tokens[3]);
            return Size{std::nullopt, *w, *h, at == std::string_view::npos ? std::string_view() : item.substr(at)};
        }
    }
    return Size{std::nullopt, std::nullopt, std::nullopt, item};
}

std::vector<std::string_view> items_of(std::string_view name) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == ',' || name[i] == '+') {
            out.push_back(name.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

std::vector<Spec> parse_name(std::string_view name, double dpi) {
    std::vector<Spec> out;
    for (const std::string_view item : items_of(name)) {
        const Size size = parse_size(item, dpi);
        const std::string_view file = trim(size.rest);
        const std::size_t dot = file.rfind('.');
        if (dot == std::string_view::npos) continue;
        const std::string_view stem = file.substr(0, dot);
        if (stem.empty()) continue;
        const std::string ext = lower(file.substr(dot + 1));
        Spec spec;
        spec.scale = size.scale;
        spec.width = size.width;
        spec.height = size.height;
        std::string out_ext;
        if (ext.starts_with("jpeg") || ext.starts_with("jpg")) {
            const std::string_view q = trim(std::string_view(ext).substr(ext.starts_with("jpeg") ? 4 : 3));
            if (!q.empty()) {
                if (q.ends_with('%')) {
                    const auto v = whole(q.substr(0, q.size() - 1));
                    if (!v) continue;
                    spec.quality = static_cast<int>(std::clamp<long long>(*v, 1, 100));
                } else {
                    const auto v = whole(q);
                    if (!v) continue;
                    spec.quality = static_cast<int>(std::clamp<long long>(*v, 1, 10) * 10);
                }
            }
            spec.format = "jpg";
            out_ext = ext.starts_with("jpeg") ? "jpeg" : "jpg";
        } else if (ext == "png" || ext == "png32") {
            spec.format = "png32";
            out_ext = "png";
        } else if (ext == "png24" || ext == "png8") {
            spec.format = ext;
            out_ext = "png";
        } else if (ext == "gif" || ext == "webp") {
            spec.format = ext;
            out_ext = ext;
        } else {
            continue;
        }
        spec.file = std::string(stem) + "." + out_ext;
        out.push_back(std::move(spec));
    }
    return out;
}

std::optional<std::vector<Default>> parse_defaults(std::string_view name, double dpi) {
    std::string_view rest = trim(name);
    if (!rest.starts_with("default")) return std::nullopt;
    rest.remove_prefix(7);
    if (!rest.empty() && !is_space(rest.front())) return std::nullopt;
    std::vector<Default> out;
    for (const std::string_view item : items_of(rest)) {
        const Size size = parse_size(item, dpi);
        const std::string_view tail = trim(size.rest);
        Default variant;
        variant.scale = size.scale;
        variant.width = size.width;
        variant.height = size.height;
        if (const std::size_t slash = tail.rfind('/'); slash != std::string_view::npos) {
            variant.folder = std::string(tail.substr(0, slash + 1));
            variant.suffix = std::string(tail.substr(slash + 1));
        } else {
            variant.suffix = std::string(tail);
        }
        if (!variant.scale && !variant.width && !variant.height && variant.folder.empty() && variant.suffix.empty()) continue;
        out.push_back(std::move(variant));
    }
    return out;
}

bool safe_relative(std::string_view path) {
    if (path.empty() || path.size() > 1024) return false;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i < path.size()) {
            const auto c = static_cast<unsigned char>(path[i]);
            if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') return false;
            if (c != '/') continue;
        }
        const std::string_view part = path.substr(start, i - start);
        if (part.empty() || part == "." || part == "..") return false;
        // (Windows: a part may not end in a dot or a space — "a." is "a" there —, nor be a device's name, whatever follows
        // its first dot: con.png, nul.png, aux/x.png, com1.jpg, lpt9.gif)
        if (part.back() == '.' || part.back() == ' ') return false;
        std::string base = lower(part.substr(0, std::min(part.find('.'), part.size())));
        while (!base.empty() && base.back() == ' ') base.pop_back();
        static const std::array<std::string_view, 4> kDevices{"con", "prn", "aux", "nul"};
        if (std::find(kDevices.begin(), kDevices.end(), base) != kDevices.end()) return false;
        if (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3] >= '1' && base[3] <= '9') return false;
        start = i + 1;
    }
    return true;
}

}  // namespace image_assets

}  // namespace genko::core
