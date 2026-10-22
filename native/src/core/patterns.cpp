#include "core/patterns.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <set>

#include "core/color_raster.hpp"
#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& message) { throw Error("value", "pattern " + message); }

bool is_ref(const std::string& ref) {
    if (ref.size() != 71 || ref.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(ref.begin() + 7, ref.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool is_id(const std::string& id) {
    return !id.empty() && id.size() <= 64 &&
           std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

const std::string& text(const Json& data, const char* key) {
    const Json* v = get(data, key);
    if (v == nullptr || !v->is_string()) bad(std::string(key) + " must be text");
    return v->get_ref<const std::string&>();
}

std::int64_t whole(const Json& data, const char* key, std::int64_t lo, std::int64_t hi) {
    const Json* v = get(data, key);
    if (v == nullptr || !v->is_number_integer() || (v->is_number_unsigned() && v->get<std::uint64_t>() > static_cast<std::uint64_t>(hi)) ||
        v->get<std::int64_t>() < lo || v->get<std::int64_t>() > hi) {
        bad(std::string(key) + " must be a whole number between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return v->get<std::int64_t>();
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

}  // namespace

Json pattern_to_json(const Pattern& p) {
    Json out = Json::object();
    out["id"] = p.id;
    out["name"] = p.name;
    out["asset"] = p.asset;
    out["mode"] = p.mode;
    out["depth"] = p.depth;
    out["width"] = p.width;
    out["height"] = p.height;
    out["dpi"] = p.dpi;
    return out;
}

Pattern pattern_from_json(const Json& data) {
    if (!data.is_object()) bad("must be an object");
    static const char* const kKeys[] = {"id", "name", "asset", "mode", "depth", "width", "height", "dpi"};
    for (const auto& [key, value] : data.items()) {
        if (std::none_of(std::begin(kKeys), std::end(kKeys), [&](const char* k) { return key == k; })) bad("has an unknown key: " + key);
    }
    Pattern p;
    p.id = text(data, "id");
    if (!is_id(p.id)) bad("id must be 1 to 64 of A-Z, a-z, 0-9, - and _");
    p.name = text(data, "name");
    const std::size_t chars = code_points(p.name).size();
    if (chars < 1 || chars > 255) bad("name must be 1 to 255 characters");
    p.asset = text(data, "asset");
    if (!is_ref(p.asset)) bad("asset must be an asset ref (sha256:<64 hex>)");
    p.mode = text(data, "mode");
    if (p.mode != "rgb" && p.mode != "gray" && p.mode != "cmyk" && p.mode != "lab") bad("mode must be one of rgb, gray, cmyk, lab");
    p.depth = static_cast<int>(whole(data, "depth", 8, 32));
    if (p.depth != 8 && p.depth != 16 && p.depth != 32) bad("depth must be 8, 16 or 32");
    p.width = whole(data, "width", 1, kMaxPatternSide);
    p.height = whole(data, "height", 1, kMaxPatternSide);
    if (p.width * p.height > kMaxPatternPixels) bad("is at most " + std::to_string(kMaxPatternPixels) + " pixels");
    const Json* dpi = get(data, "dpi");
    const double d = dpi != nullptr && dpi->is_number() ? dpi->get<double>() : std::nan("");
    if (!(std::isfinite(d) && d >= 50.0 && d <= 2400.0)) bad("dpi must be a number between 50 and 2400");
    p.dpi = d;
    return p;
}

std::string_view pattern_suffix(const Pattern& p) {
    if (p.depth == 8 && (p.mode == "rgb" || p.mode == "gray")) return ".png";
    if (p.mode == "rgb") return kColorRasterSuffix;
    return kInkRasterSuffix;
}

std::optional<std::string> check_pattern_file(const Pattern& p, std::string_view bytes) {
    const std::string_view suffix = pattern_suffix(p);
    const auto size_of = [&](std::uint32_t w, std::uint32_t h) -> std::optional<std::string> {
        if (static_cast<std::int64_t>(w) != p.width || static_cast<std::int64_t>(h) != p.height) {
            return "its picture is " + std::to_string(w) + " × " + std::to_string(h) + ", not " + std::to_string(p.width) + " × " +
                   std::to_string(p.height);
        }
        return std::nullopt;
    };
    if (suffix == ".png") {
        static constexpr unsigned char kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        if (bytes.size() < 33 || !std::equal(std::begin(kSignature), std::end(kSignature), reinterpret_cast<const unsigned char*>(bytes.data())) ||
            bytes.substr(12, 4) != "IHDR") {
            return std::string("its picture is not a PNG");
        }
        const unsigned depth = static_cast<unsigned char>(bytes[24]), type = static_cast<unsigned char>(bytes[25]);
        if (depth != 8 || type != (p.mode == "rgb" ? 6u : 4u)) return std::string(p.mode == "rgb" ? "its picture is not an 8-bit RGBA PNG" : "its picture is not an 8-bit LA PNG");
        return size_of(be32(bytes, 16), be32(bytes, 20));
    }
    try {
        if (suffix == kColorRasterSuffix) {
            const ColorRasterView view(bytes);
            const bool floating = view.metadata("")["precision"] == "f32";
            if (floating != (p.depth == 32)) return std::string("its picture is not of ") + std::to_string(p.depth) + " bits";
            return size_of(view.width(), view.height());
        }
        const InkRasterHeader header = ink_raster_header(bytes);
        const InkSpace space = p.mode == "cmyk" ? InkSpace::Cmyk : p.mode == "lab" ? InkSpace::Lab : InkSpace::Gray;
        if (header.space != space || header.depth != p.depth) return std::string("its picture is not of its mode and depth");
        return size_of(header.width, header.height);
    } catch (const Error& error) {
        return std::string("its picture cannot be read: ") + error.what();
    }
}

bool uses_patterns(const Document& doc) { return !doc.patterns.empty(); }

void validate_patterns(const Document& doc) {
    if (doc.patterns.size() > kMaxPatterns) throw Error("value", "a book keeps at most " + std::to_string(kMaxPatterns) + " patterns");
    std::set<std::string> ids;
    for (const Pattern& p : doc.patterns) {
        try {
            (void)pattern_from_json(pattern_to_json(p));
        } catch (const Error& error) {
            throw Error("value", std::string(error.what()) + " (" + p.id + ")");
        }
        if (!ids.insert(p.id).second) throw Error("value", "the pattern id " + p.id + " is there twice");
        const auto file = doc.pattern_files.find(p.asset);
        if (file == doc.pattern_files.end() || !file->second) throw Error("value", "the picture " + p.asset + " of the pattern " + p.id + " is not held");
        if (const auto why = check_pattern_file(p, *file->second)) throw Error("value", "the pattern " + p.id + ": " + *why);
    }
}

const Pattern* find_pattern(const Document& doc, std::string_view id) {
    for (const Pattern& p : doc.patterns) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

}  // namespace genko::core
