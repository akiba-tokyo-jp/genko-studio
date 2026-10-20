// A PSD's placed layers and linked files (render/psd_smart.hpp, PC3 A3). Upstream PhotoCraft v0.2.0's
// io/src/blocks.rs (344-421: parse_placed_warp, parse_smart) and io/src/linked.rs (1-128: the lnk2 items read and a
// liFD item written; MIT, see third_party/photocraft/NOTICE.md); PlLd and the fresh SoLd are written as Photoshop
// writes them and psd-tools reads them.

#include "render/psd_smart.hpp"

#include <algorithm>
#include <cmath>

#include "core/ink.hpp"
#include "core/page_geometry.hpp"
#include "render/psd_descriptor.hpp"

namespace genko::render::psd {

using core::Json;

namespace {

using descriptor::Broken;
using descriptor::Bytes;
using descriptor::Object;
using descriptor::Value;

constexpr std::array<std::string_view, 17> kStyles{"none",       "custom",     "arc",  "arcLower", "arcUpper", "arch",
                                                   "bulge",      "shellLower", "shellUpper", "flag", "wave", "fish",
                                                   "rise",       "fisheye",    "inflate",    "squeeze", "twist"};

const Object* object_of(const Object& d, std::string_view key) {
    const Value* v = d.get(key);
    return v != nullptr && v->object && (v->type == "Objc" || v->type == "GlbO" || v->type == "ObAr") ? v->object.get() : nullptr;
}

std::optional<double> number_of(const Object& d, std::string_view key) {
    const Value* v = d.get(key);
    if (v == nullptr) return std::nullopt;
    const auto n = v->numeric();
    if (!n || !std::isfinite(*n)) return std::nullopt;
    return n;
}

// Eight numbers of a list (Trnf, nonAffineTransform), each finite; none otherwise.
std::optional<std::array<double, 8>> corners_of(const Object& d, std::string_view key) {
    const Value* v = d.get(key);
    if (v == nullptr || v->type != "VlLs" || v->list.size() != 8) return std::nullopt;
    std::array<double, 8> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        const auto n = v->list[i].numeric();
        if (!n || !std::isfinite(*n)) return std::nullopt;
        out[i] = *n;
    }
    return out;
}

// The style's name as Genko holds it ("warpArcLower" → "arcLower"), none for one it does not know.
std::optional<std::string> style_name(std::string_view psd) {
    if (!psd.starts_with("warp") || psd.size() <= 4) return std::nullopt;
    std::string name(psd.substr(4));
    if (name[0] >= 'A' && name[0] <= 'Z') name[0] = static_cast<char>(name[0] - 'A' + 'a');
    if (std::find(kStyles.begin(), kStyles.end(), name) == kStyles.end()) return std::nullopt;
    return name;
}

// A warp descriptor (upstream parse_placed_warp) in Genko's form; none for none, or one that changes nothing.
std::optional<Json> warp_of(const Object& w) {
    const Value* style = w.get("warpStyle");
    if (style == nullptr || style->type != "enum") return std::nullopt;
    const auto name = style_name(style->enum_value);
    if (!name || *name == "none") return std::nullopt;
    const Object* box = object_of(w, "bounds");
    if (box == nullptr) return std::nullopt;
    const auto left = number_of(*box, "Left"), top = number_of(*box, "Top "), right = number_of(*box, "Rght"), bottom = number_of(*box, "Btom");
    if (!left || !top || !right || !bottom) return std::nullopt;
    Json out = Json::object();
    out["style"] = *name;
    out["bend"] = number_of(w, "warpValue").value_or(0.0);
    out["h_distort"] = number_of(w, "warpPerspective").value_or(0.0);
    out["v_distort"] = number_of(w, "warpPerspectiveOther").value_or(0.0);
    const Value* rotate = w.get("warpRotate");
    out["vertical"] = rotate != nullptr && rotate->type == "enum" && rotate->enum_value == "Vrtc";
    out["bounds"] = Json::array({*left, *top, *right, *bottom});
    if (*name == "custom") {
        const Object* envelope = object_of(w, "customEnvelopeWarp");
        const Object* points = envelope != nullptr ? object_of(*envelope, "meshPoints") : nullptr;
        if (points == nullptr) return std::nullopt;
        const Value* xs = points->get("Hrzn");
        const Value* ys = points->get("Vrtc");
        if (xs == nullptr || ys == nullptr || xs->type != "UnFl" || ys->type != "UnFl" || xs->numbers.size() != 16 || ys->numbers.size() != 16)
            return std::nullopt;
        Json mesh = Json::array();
        bool identity = true;
        for (std::size_t k = 0; k < 16; ++k) {
            const double x = xs->numbers[k], y = ys->numbers[k];
            if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
            const double gx = *left + (*right - *left) * static_cast<double>(k % 4) / 3.0;
            const double gy = *top + (*bottom - *top) * static_cast<double>(k / 4) / 3.0;
            identity = identity && std::abs(x - gx) < 1e-9 && std::abs(y - gy) < 1e-9;
            mesh.push_back(Json::array({x, y}));
        }
        if (identity) return std::nullopt;
        out["mesh"] = Json{{"us", Json::array({0, 1})}, {"vs", Json::array({0, 1})}, {"points", std::move(mesh)}};
        return out;
    }
    // (a style at no bend and no distortion leaves the contents as they are)
    if (out["bend"].get<double>() == 0 && out["h_distort"].get<double>() == 0 && out["v_distort"].get<double>() == 0) return std::nullopt;
    return out;
}

std::uint64_t be64(std::string_view b, std::size_t at) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v = (v << 8) | static_cast<unsigned char>(b[at + i]);
    return v;
}

std::optional<LinkedItem> parse_item(std::string_view item) {
    try {
        Bytes b(item);
        LinkedItem out;
        out.item = item;
        out.kind = std::string(b.take(4));
        out.version = b.u32();
        if (out.version < 1 || out.version > 7) return std::nullopt;
        const std::uint32_t n = b.u8();
        out.uuid = std::string(b.take(n));
        out.name = utf8_of(descriptor::read_unicode(b));
        out.type = std::string(b.take(4));
        (void)b.take(4);  // (its creator)
        const std::uint64_t high = b.u32();
        const std::uint64_t length = (high << 32) | b.u32();
        if (b.u8() != 0) {  // (the open descriptor: a version and a descriptor)
            (void)b.u32();
            (void)descriptor::read_structure(b, 0);
        }
        if (out.kind == "liFD") {
            if (length > b.left()) return std::nullopt;
            out.data = item.substr(b.at(), static_cast<std::size_t>(length));
        }
        return out;
    } catch (const Broken&) {
        return std::nullopt;
    }
}

void pad4(std::string& out) { out.append((4 - out.size() % 4) % 4, '\0'); }

// The warp descriptor written (warpNone without one; a custom warp's 4 × 4 mesh as Photoshop's meshPoints).
descriptor::Writer warp_writer(const std::optional<Json>& warp, double width, double height) {
    descriptor::Writer w;
    const auto number = [&](const char* key, double fallback) {
        if (!warp || !warp->contains(key) || !(*warp)[key].is_number()) return fallback;
        return (*warp)[key].get<double>();
    };
    std::string style = warp && warp->contains("style") && (*warp)["style"].is_string() ? (*warp)["style"].get<std::string>() : std::string("none");
    if (std::find(kStyles.begin(), kStyles.end(), style) == kStyles.end()) style = "none";
    std::string psd = "warp" + style;
    psd[4] = static_cast<char>(psd[4] - 'a' + 'A');
    w.enumerated("warpStyle", "warpStyle", psd);
    w.number("warpValue", number("bend", 0));
    w.number("warpPerspective", number("h_distort", 0));
    w.number("warpPerspectiveOther", number("v_distort", 0));
    const bool vertical = warp && warp->contains("vertical") && (*warp)["vertical"].is_boolean() && (*warp)["vertical"].get<bool>();
    w.enumerated("warpRotate", "Ornt", vertical ? "Vrtc" : "Hrzn");
    std::array<double, 4> box{0, 0, width, height};
    if (warp && warp->contains("bounds") && (*warp)["bounds"].is_array() && (*warp)["bounds"].size() == 4) {
        for (std::size_t i = 0; i < 4; ++i) {
            if ((*warp)["bounds"][i].is_number()) box[i] = (*warp)["bounds"][i].get<double>();
        }
    }
    descriptor::Writer bounds;
    bounds.unit("Top ", "#Pxl", box[1]);
    bounds.unit("Left", "#Pxl", box[0]);
    bounds.unit("Btom", "#Pxl", box[3]);
    bounds.unit("Rght", "#Pxl", box[2]);
    w.object("bounds", bounds, "Rctn");
    w.integer("uOrder", 4);
    w.integer("vOrder", 4);
    if (style == "custom" && warp && warp->contains("mesh") && (*warp)["mesh"].contains("points")) {
        std::vector<double> xs, ys;
        const Json& mesh = (*warp)["mesh"];
        core::page_geometry::BezierMesh patches;
        for (const auto& [key, list] : {std::pair{"us", &patches.us}, std::pair{"vs", &patches.vs}}) {
            if (!mesh.contains(key) || !mesh[key].is_array()) continue;
            for (const Json& v : mesh[key])
                if (v.is_number()) list->push_back(v.get<double>());
        }
        for (const Json& point : mesh["points"]) {
            if (!point.is_array() || point.size() != 2) continue;
            xs.push_back(point[0].get<double>());
            ys.push_back(point[1].get<double>());
            patches.points.push_back(core::vector::XY{point[0].get<double>(), point[1].get<double>()});
        }
        if (patches.valid() && (patches.us.size() > 2 || patches.vs.size() > 2)) {
            // (PC3 A5: a warp split into patches as Photoshop's one patch of 4 × 4 points fitted to it — its split form,
            // quiltWarp, is not one this build can check against Photoshop)
            const auto one = core::page_geometry::BezierMesh::fit([&patches](double s, double t) { return patches.eval(s, t); }, {0, 1}, {0, 1});
            xs.clear();
            ys.clear();
            for (const core::vector::XY& p : one.points) {
                xs.push_back(p.x);
                ys.push_back(p.y);
            }
        }
        descriptor::Writer envelope;
        envelope.object_array("meshPoints", static_cast<std::uint32_t>(xs.size()), "rationalPoint", {{"Hrzn", xs}, {"Vrtc", ys}}, "#Pxl");
        w.object("customEnvelopeWarp", envelope, "customEnvelopeWarp");
    }
    return w;
}

}  // namespace

std::optional<Placed> read_placed(std::string_view key, std::string_view data) {
    try {
        Placed out;
        out.key = std::string(key);
        if (key == "SoLd" || key == "SoLE") {
            // (soLD, its version — 4 or 5 —, a versioned descriptor)
            if (data.size() < 8 || data.substr(0, 4) != "soLD") return std::nullopt;
            Bytes head(data.substr(4, 4));
            const std::uint32_t version = head.u32();
            if (version != 4 && version != 5) return std::nullopt;
            const Object d = descriptor::read_versioned(data, 8);
            const Value* id = d.get("Idnt");
            if (id == nullptr || id->type != "TEXT" || id->text.empty()) return std::nullopt;
            out.uuid = utf8_of(id->text);
            if (const Value* placed = d.get("placed"); placed != nullptr && placed->type == "TEXT") out.placed = utf8_of(placed->text);
            auto corners = corners_of(d, "nonAffineTransform");
            if (!corners) corners = corners_of(d, "Trnf");
            if (corners) {
                out.corners = *corners;
                out.has_corners = true;
            }
            if (const Object* size = object_of(d, "Sz  ")) {
                out.width = std::max(0.0, number_of(*size, "Wdth").value_or(0.0));
                out.height = std::max(0.0, number_of(*size, "Hght").value_or(0.0));
            }
            out.resolution = std::max(0.0, number_of(d, "Rslt").value_or(0.0));
            out.type = static_cast<int>(number_of(d, "Type").value_or(0.0));
            if (const Object* warp = object_of(d, "warp")) out.warp = warp_of(*warp);
            out.filters = d.get("filterFX") != nullptr;
            return out;
        }
        if (key == "PlLd") {
            // (plcL, version 3, the id as a Pascal string, page, pages, anti-aliasing, type, the corners as 8 doubles, the warp)
            Bytes b(data);
            if (b.take(4) != "plcL" || b.u32() != 3) return std::nullopt;
            const std::uint32_t n = b.u8();
            out.uuid = std::string(b.take(n));
            if (out.uuid.empty()) return std::nullopt;
            (void)b.u32();
            (void)b.u32();
            (void)b.u32();
            out.type = static_cast<int>(b.u32());
            bool finite = true;
            for (double& v : out.corners) {
                v = b.f64();
                finite = finite && std::isfinite(v);
            }
            out.has_corners = finite;
            try {
                (void)b.u32();  // (the warp's version)
                const std::size_t at = b.at();
                const Object warp = descriptor::read_versioned(data, at);
                out.warp = warp_of(warp);
            } catch (const Broken&) {
                // (its warp unread: none)
            }
            return out;
        }
        return std::nullopt;
    } catch (const Broken&) {
        return std::nullopt;
    }
}

bool parallelogram(const std::array<double, 8>& p, double tolerance) {
    // (top left + bottom right = top right + bottom left)
    return std::abs(p[0] + p[4] - p[2] - p[6]) <= tolerance && std::abs(p[1] + p[5] - p[3] - p[7]) <= tolerance;
}

std::vector<LinkedItem> read_linked(std::string_view block) {
    std::vector<LinkedItem> out;
    std::size_t at = 0;
    while (block.size() >= 8 && at <= block.size() - 8) {
        const std::uint64_t length = be64(block, at);
        at += 8;
        if (length > block.size() - at) break;
        const std::string_view item = block.substr(at, static_cast<std::size_t>(length));
        at += static_cast<std::size_t>(length);
        at = std::min(block.size(), (at + 3) / 4 * 4);
        if (auto parsed = parse_item(item)) out.push_back(*parsed);
    }
    return out;
}

std::string framed_item(std::string_view item) {
    std::string out;
    const auto size = static_cast<std::uint64_t>(item.size());
    descriptor::put_u32(out, static_cast<std::uint32_t>(size >> 32));
    descriptor::put_u32(out, static_cast<std::uint32_t>(size & 0xffffffffULL));
    out += item;
    pad4(out);
    return out;
}

std::string linked_item(std::string_view uuid, std::string_view name, std::string_view type, std::string_view bytes) {
    std::string item = "liFD";
    descriptor::put_u32(item, 2);
    const std::size_t n = std::min<std::size_t>(uuid.size(), 255);
    descriptor::put_u8(item, static_cast<unsigned>(n));
    item += uuid.substr(0, n);
    const std::u16string units = utf16(name);
    descriptor::put_u32(item, static_cast<std::uint32_t>(units.size()));
    for (const char16_t u : units) descriptor::put_u16(item, u);
    std::string kind(type.substr(0, 4));
    kind.resize(4, ' ');
    item += kind;
    item += "8BIM";
    const auto size = static_cast<std::uint64_t>(bytes.size());
    descriptor::put_u32(item, static_cast<std::uint32_t>(size >> 32));
    descriptor::put_u32(item, static_cast<std::uint32_t>(size & 0xffffffffULL));
    descriptor::put_u8(item, 0);  // (no open descriptor)
    item += bytes;
    return framed_item(item);
}

std::string file_type(std::string_view format) {
    if (format == "png") return "png ";
    if (format == "jpeg") return "JPEG";
    if (format == "tiff") return "TIFF";
    if (format == "webp") return "WEBP";
    if (format == "bmp") return "BMP ";
    if (format == "gif") return "GIFf";
    if (format == "psb") return "8BPB";
    return "8BPS";
}

std::string sold_block(const PlacedOut& p) {
    descriptor::Writer d;
    d.text("Idnt", utf16(p.uuid));
    d.text("placed", utf16(p.placed));
    d.integer("PgNm", 1);
    d.integer("totalPages", 1);
    descriptor::Writer step;
    step.integer("numerator", 0);
    step.integer("denominator", 600);
    d.object("frameStep", step, "null");
    d.object("duration", step, "null");
    d.integer("frameCount", 1);
    d.integer("Annt", 16);
    d.integer("Type", p.type);
    const std::vector<double> corners(p.corners.begin(), p.corners.end());
    d.numbers("Trnf", corners);
    d.numbers("nonAffineTransform", corners);
    d.object("warp", warp_writer(p.warp, p.width, p.height), "warp");
    descriptor::Writer size;
    size.number("Wdth", p.width);
    size.number("Hght", p.height);
    d.object("Sz  ", size, "Pnt ");
    d.unit("Rslt", "#Rsl", p.resolution);
    d.integer("comp", -1);
    descriptor::Writer comp;
    comp.integer("compID", -1);
    comp.integer("originalCompID", -1);
    d.object("compInfo", comp, "null");
    std::string out = "soLD";
    descriptor::put_u32(out, 4);
    out += d.versioned("null");
    pad4(out);
    return out;
}

std::string plld_block(const PlacedOut& p) {
    std::string out = "plcL";
    descriptor::put_u32(out, 3);
    const std::size_t n = std::min<std::size_t>(p.uuid.size(), 255);
    descriptor::put_u8(out, static_cast<unsigned>(n));
    out += p.uuid.substr(0, n);
    descriptor::put_u32(out, 1);   // (its page)
    descriptor::put_u32(out, 1);   // (pages)
    descriptor::put_u32(out, 16);  // (anti-aliased)
    descriptor::put_u32(out, static_cast<std::uint32_t>(p.type));
    for (const double v : p.corners) descriptor::put_f64(out, v);
    descriptor::put_u32(out, 0);  // (the warp's version)
    out += warp_writer(p.warp, p.width, p.height).versioned("warp");
    pad4(out);
    return out;
}

std::string uuid_of(std::string_view seed) {
    const std::string hex = core::bytes_ref(seed).substr(7, 32);
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

}  // namespace genko::render::psd
