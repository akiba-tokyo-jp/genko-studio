#include "render/psd_paths.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

// PhotoCraft v0.2.0 crates/psd/src/path.rs (1-298) and io/src/vector_map.rs (1-130), MIT:
// native/third_party/photocraft/NOTICE.md.

namespace genko::render::psd_paths {

namespace vec = core::vector;

namespace {

constexpr std::size_t kRecord = 26;
constexpr unsigned kClosedLength = 0, kClosedLinked = 1, kClosedUnlinked = 2, kOpenLength = 3, kOpenLinked = 4, kOpenUnlinked = 5;
constexpr unsigned kFillRule = 6, kInitialFill = 8;
constexpr std::uint32_t kInvert = 1, kNotLinked = 2, kDisabled = 4;

void put_u16(std::string& out, std::uint32_t v) {
    out += static_cast<char>((v >> 8) & 0xff);
    out += static_cast<char>(v & 0xff);
}
void put_u32(std::string& out, std::uint32_t v) {
    put_u16(out, v >> 16);
    put_u16(out, v & 0xffff);
}
std::uint32_t u16_at(std::string_view d, std::size_t at) {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(d[at])) << 8) | static_cast<unsigned char>(d[at + 1]);
}
std::uint32_t u32_at(std::string_view d, std::size_t at) { return (u16_at(d, at) << 16) | u16_at(d, at + 2); }
std::int32_t i32_at(std::string_view d, std::size_t at) { return static_cast<std::int32_t>(u32_at(d, at)); }

// A fraction as 8.24 fixed (rounded, saturating), and back.
std::int32_t fixed(double v) {
    const double scaled = std::round(v * 16777216.0);
    if (!std::isfinite(scaled)) return 0;
    return static_cast<std::int32_t>(std::clamp(scaled, static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                                                static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}
double unfixed(std::int32_t v) { return static_cast<double>(v) / 16777216.0; }

std::int16_t op_to_psd(vec::PathOp op) {
    switch (op) {
        case vec::PathOp::Combine: return 1;
        case vec::PathOp::Subtract: return 2;
        case vec::PathOp::Intersect: return 3;
        case vec::PathOp::Exclude: return 0;
    }
    return 1;
}
vec::PathOp op_from_psd(std::int16_t v) {
    switch (v) {
        case 0: return vec::PathOp::Exclude;
        case 2: return vec::PathOp::Subtract;
        case 3: return vec::PathOp::Intersect;
        default: return vec::PathOp::Combine;
    }
}

std::string record(unsigned selector, const std::string& data) {
    std::string out;
    put_u16(out, selector);
    out += data;
    out.resize(kRecord, '\0');
    return out;
}

// A resource: "8BIM", its id, its name (a Pascal string padded to even), its size and its data padded to even.
std::string resource(unsigned id, const std::string& name, const std::string& data) {
    std::string out = "8BIM";
    put_u16(out, id);
    const std::string raw = resource_name(name);
    out += static_cast<char>(raw.size());
    out += raw;
    if ((raw.size() + 1) % 2) out += '\0';
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    out += data;
    if (data.size() % 2) out += '\0';
    return out;
}

}  // namespace

std::string records(const vec::Path& path, double width_mm, double height_mm) {
    const double w = width_mm > 0 ? width_mm : 1.0, h = height_mm > 0 ? height_mm : 1.0;
    std::string out = record(kFillRule, {});
    std::string initial;
    put_u16(initial, path.inverted ? 1 : 0);
    out += record(kInitialFill, initial);
    for (std::size_t i = 0; i < path.subpaths.size(); ++i) {
        const vec::Subpath& s = path.subpaths[i];
        const std::size_t count = std::min<std::size_t>(s.knots.size(), 0xffff);
        std::string length;
        put_u16(length, static_cast<std::uint32_t>(count));
        put_u16(length, static_cast<std::uint16_t>(op_to_psd(path.effective_op(i))));
        put_u16(length, 1);  // (Photoshop writes 1 here, as psd-tools does)
        out += record(s.closed ? kClosedLength : kOpenLength, length);
        for (std::size_t k = 0; k < count; ++k) {
            const vec::Knot& knot = s.knots[k];
            std::string data;
            for (const vec::XY& p : {knot.in, knot.anchor, knot.out}) {
                put_u32(data, static_cast<std::uint32_t>(fixed(p.y / h)));
                put_u32(data, static_cast<std::uint32_t>(fixed(p.x / w)));
            }
            const unsigned selector = s.closed ? (knot.smooth ? kClosedLinked : kClosedUnlinked) : (knot.smooth ? kOpenLinked : kOpenUnlinked);
            out += record(selector, data);
        }
    }
    return out;
}

std::optional<vec::Path> path_of(std::string_view data, double width_mm, double height_mm) {
    if (data.size() < kRecord) return std::nullopt;
    vec::Path path;
    std::size_t remaining = 0;
    for (std::size_t at = 0; at + kRecord <= data.size(); at += kRecord) {
        const unsigned selector = u16_at(data, at);
        const std::string_view d = data.substr(at + 2, kRecord - 2);
        if (selector == kClosedLength || selector == kOpenLength) {
            remaining = u16_at(d, 0);
            vec::Subpath s;
            s.closed = selector == kClosedLength;
            s.op = op_from_psd(static_cast<std::int16_t>(u16_at(d, 2)));
            path.subpaths.push_back(std::move(s));
        } else if (selector == kClosedLinked || selector == kClosedUnlinked || selector == kOpenLinked || selector == kOpenUnlinked) {
            const bool closed = selector <= kClosedUnlinked;
            if (remaining == 0 && (path.subpaths.empty() || path.subpaths.back().closed != closed)) {
                vec::Subpath s;
                s.closed = closed;
                path.subpaths.push_back(std::move(s));
            }
            if (remaining > 0) --remaining;
            const auto point = [&](std::size_t i) { return vec::XY{unfixed(i32_at(d, i * 8 + 4)) * width_mm, unfixed(i32_at(d, i * 8)) * height_mm}; };
            vec::Knot knot{point(1), point(0), point(2), selector == kClosedLinked || selector == kOpenLinked};
            if (path.subpaths.back().knots.size() >= vec::kKnotsMax) return std::nullopt;
            path.subpaths.back().knots.push_back(knot);
        } else if (selector == kInitialFill) {
            path.inverted = u16_at(d, 0) != 0;
        }
    }
    if (!path.subpaths.empty()) path.subpaths.front().op = vec::PathOp::Combine;
    std::erase_if(path.subpaths, [](const vec::Subpath& s) { return s.knots.empty(); });
    return path;
}

std::string vmsk(const vec::VectorMask& mask, double width_mm, double height_mm) {
    vec::Path path = mask.path;
    std::uint32_t flags = 0;
    if (path.inverted) {
        flags |= kInvert;
        path.inverted = false;
    }
    if (!mask.linked) flags |= kNotLinked;
    if (!mask.enabled) flags |= kDisabled;
    std::string out;
    put_u32(out, 3);
    put_u32(out, flags);
    return out + records(path, width_mm, height_mm);
}

std::optional<vec::VectorMask> vector_mask_of(std::string_view data, double width_mm, double height_mm) {
    if (data.size() < 8) return std::nullopt;
    const std::uint32_t flags = u32_at(data, 4);
    vec::VectorMask mask;
    if (data.size() >= 8 + kRecord) {
        std::optional<vec::Path> path = path_of(data.substr(8), width_mm, height_mm);
        if (!path) return std::nullopt;
        mask.path = std::move(*path);
    }
    if (flags & kInvert) mask.path.inverted = !mask.path.inverted;
    mask.linked = (flags & kNotLinked) == 0;
    mask.enabled = (flags & kDisabled) == 0;
    return mask;
}

std::string resource_name(const std::string& name) {
    std::string out;
    for (std::size_t at = 0; at < name.size();) {
        const auto lead = static_cast<unsigned char>(name[at]);
        const std::size_t n = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        if (out.size() + n > 255) break;
        out += name.substr(at, n);
        at += n;
    }
    return out;
}

std::string name_of(std::string_view bytes) {
    // (UTF-8 when it is: the names this writes; else each byte a Latin-1 letter)
    bool utf8 = true;
    for (std::size_t at = 0; at < bytes.size() && utf8;) {
        const auto lead = static_cast<unsigned char>(bytes[at]);
        const std::size_t n = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 0;
        if (n == 0 || at + n > bytes.size()) {
            utf8 = false;
            break;
        }
        for (std::size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(bytes[at + k]) & 0xc0) != 0x80) utf8 = false;
        at += n;
    }
    if (utf8) return std::string(bytes);
    std::string out;
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) {
            out += ch;
        } else {
            out += static_cast<char>(0xc0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3f));
        }
    }
    return out;
}

std::string clipping(const vec::ClippingPath& clip) {
    const std::string raw = resource_name(clip.name);
    std::string out(1, static_cast<char>(raw.size()));
    out += raw;
    const double flatness = std::clamp(clip.flatness, 0.0, 100.0);
    put_u32(out, static_cast<std::uint32_t>(std::lround(flatness * 65536.0)));
    return out;
}

std::optional<vec::ClippingPath> clipping_of(std::string_view data) {
    if (data.empty()) return std::nullopt;
    const std::size_t n = static_cast<unsigned char>(data[0]);
    if (1 + n > data.size()) return std::nullopt;
    vec::ClippingPath clip;
    clip.name = name_of(data.substr(1, n));
    if (1 + n + 4 <= data.size()) clip.flatness = u32_at(data, 1 + n) / 65536.0;
    return clip;
}

std::string resources(const core::Page& page) {
    const double w = page.spec.width_mm.value(), h = page.spec.height_mm.value();
    std::string out;
    const std::size_t saved = std::min<std::size_t>(page.paths.size(), kSavedLast - kSavedFirst + 1);
    for (std::size_t i = 0; i < saved; ++i)
        out += resource(kSavedFirst + static_cast<unsigned>(i), page.paths[i].name, records(page.paths[i].path, w, h));
    if (page.work_path) out += resource(kWorkPath, {}, records(*page.work_path, w, h));
    if (page.clipping_path) out += resource(kClippingPath, {}, clipping(*page.clipping_path));
    return out;
}

}  // namespace genko::render::psd_paths
