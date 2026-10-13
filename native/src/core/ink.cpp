#include "core/ink.hpp"

#include <zlib.h>

#include <QCryptographicHash>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", "the page's colour mode cannot be read: " + why); }
[[noreturn]] void bad_raster(const std::string& why) { throw Error("format", "the ink raster cannot be read: " + why); }

bool is_ref(const std::string& text) {
    if (text.size() != 71 || text.compare(0, 7, "sha256:") != 0) return false;
    return std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// A number of the JSON that is a number and not a bool (nlohmann keeps them apart), finite.
std::optional<double> number(const Json& v) {
    if (!v.is_number()) return std::nullopt;
    const double d = v.get<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return d;
}

std::uint32_t le32(std::string_view b, std::size_t at) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= std::uint32_t(static_cast<unsigned char>(b[at + i])) << (8 * i);
    return v;
}

void put_le32(std::string& b, std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) b.push_back(static_cast<char>((v >> (8 * i)) & 255));
}

constexpr std::size_t kHeader = 16;

// PNG's Paeth predictor.
inline unsigned paeth(unsigned a, unsigned b, unsigned c) {
    const int p = int(a) + int(b) - int(c);
    const int pa = std::abs(p - int(a)), pb = std::abs(p - int(b)), pc = std::abs(p - int(c));
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

// The row's bytes as filter `type` gives them (PNG §9), from the row and the one above (zero for the first).
void filter_row(int type, const unsigned char* row, const unsigned char* above, std::size_t n, std::size_t bpp, unsigned char* out) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned a = i >= bpp ? row[i - bpp] : 0;
        const unsigned b = above ? above[i] : 0;
        const unsigned c = (i >= bpp && above) ? above[i - bpp] : 0;
        unsigned p = 0;
        switch (type) {
            case 1: p = a; break;
            case 2: p = b; break;
            case 3: p = (a + b) / 2; break;
            case 4: p = paeth(a, b, c); break;
            default: p = 0; break;
        }
        out[i] = static_cast<unsigned char>((row[i] - p) & 255);
    }
}

// The row back from its filtered bytes, in place (`row` holds them; `above` is the row above, decoded).
void unfilter_row(int type, unsigned char* row, const unsigned char* above, std::size_t n, std::size_t bpp) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned a = i >= bpp ? row[i - bpp] : 0;
        const unsigned b = above ? above[i] : 0;
        const unsigned c = (i >= bpp && above) ? above[i - bpp] : 0;
        unsigned p = 0;
        switch (type) {
            case 1: p = a; break;
            case 2: p = b; break;
            case 3: p = (a + b) / 2; break;
            case 4: p = paeth(a, b, c); break;
            default: p = 0; break;
        }
        row[i] = static_cast<unsigned char>((row[i] + p) & 255);
    }
}

// The bytes one row of samples takes (checked against the limits).
std::size_t row_bytes(std::uint32_t width, unsigned channels, int depth) {
    return std::size_t(width) * channels * (depth == 16 ? 2 : 1);
}

void check_size(std::uint32_t width, std::uint32_t height, unsigned channels, int depth) {
    if (width < 1 || height < 1 || width > kInkRasterMaxSide || height > kInkRasterMaxSide) bad_raster("its size is out of range");
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (pixels > kInkRasterMaxPixels) bad_raster("it has too many pixels");
    if (pixels * channels * (depth == 16 ? 2u : 1u) > kInkRasterMaxBytes) bad_raster("it is too large");
}

}  // namespace

std::string_view to_string(InkSpace space) { return space == InkSpace::Cmyk ? "cmyk" : "lab"; }

std::optional<InkSpace> ink_space_from(std::string_view text) {
    if (text == "cmyk") return InkSpace::Cmyk;
    if (text == "lab") return InkSpace::Lab;
    return std::nullopt;
}

unsigned colour_channels(InkSpace space) { return space == InkSpace::Cmyk ? 4u : 3u; }

bool is_intent(std::string_view intent) { return std::find(kIntents.begin(), kIntents.end(), intent) != kIntents.end(); }

void check_ink(InkSpace space, const std::vector<double>& ink) {
    if (ink.size() != colour_channels(space)) {
        bad(std::string("an ink of a ") + (space == InkSpace::Cmyk ? "CMYK page has 4 values (c, m, y, k)" : "Lab page has 3 values (L, a, b)"));
    }
    for (std::size_t i = 0; i < ink.size(); ++i) {
        const double v = ink[i];
        if (!std::isfinite(v)) bad("an ink value is not a finite number");
        if (space == InkSpace::Cmyk && !(v >= 0 && v <= 1)) bad("a CMYK ink value is 0..1");
        if (space == InkSpace::Lab && i == 0 && !(v >= 0 && v <= 100)) bad("a Lab L is 0..100");
        if (space == InkSpace::Lab && i > 0 && !(v >= -128 && v <= 127)) bad("a Lab a or b is -128..127");
    }
}

Json to_json(const PageColor& color) {
    Json out = Json::object();
    out["mode"] = std::string(to_string(color.space));
    out["depth"] = color.depth;
    if (color.space == InkSpace::Cmyk) out["profile"] = color.profile;
    out["intent"] = color.intent;
    Json inks = Json::array();
    for (const InkEntry& entry : color.inks) {
        Json ink = Json::array();
        for (const double v : entry.ink) ink.push_back(v);
        inks.push_back(Json::array({Json::array({entry.rgb[0], entry.rgb[1], entry.rgb[2]}), std::move(ink)}));
    }
    out["inks"] = std::move(inks);
    return out;
}

PageColor page_color_from_json(const Json& value) {
    if (!value.is_object()) bad("it must be an object");
    static const std::set<std::string> kKeys{"mode", "depth", "profile", "intent", "inks"};
    for (const auto& [key, v] : value.items()) {
        if (!kKeys.contains(key)) bad("unknown key " + py_repr_str(key));
    }
    PageColor out;
    const auto mode = value.find("mode");
    if (mode == value.end() || !mode->is_string()) bad("mode is cmyk or lab");
    const auto space = ink_space_from(mode->get<std::string>());
    if (!space) bad("mode is cmyk or lab");
    out.space = *space;
    const auto depth = value.find("depth");
    if (depth == value.end() || !depth->is_number_integer() || (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16)) {
        bad("depth is 8 or 16");
    }
    out.depth = static_cast<int>(depth->get<std::int64_t>());
    const auto profile = value.find("profile");
    if (out.space == InkSpace::Cmyk) {
        if (profile == value.end() || !profile->is_string() || !is_ref(profile->get<std::string>())) {
            bad("a CMYK page names its ICC profile (sha256:<64 hex>)");
        }
        out.profile = profile->get<std::string>();
    } else if (profile != value.end()) {
        bad("a Lab page has no profile");
    }
    const auto intent = value.find("intent");
    if (intent == value.end() || !intent->is_string() || !is_intent(intent->get<std::string>())) {
        bad("intent is perceptual, relative, saturation or absolute");
    }
    out.intent = intent->get<std::string>();
    const auto inks = value.find("inks");
    if (inks == value.end() || !inks->is_array()) bad("inks is a list");
    if (inks->size() > kInksMax) bad("a page has at most " + std::to_string(kInksMax) + " inks");
    std::set<std::array<int, 3>> seen;
    for (const Json& item : *inks) {
        if (!item.is_array() || item.size() != 2 || !item[0].is_array() || item[0].size() != 3 || !item[1].is_array()) {
            bad("each ink is [[r, g, b], [values]]");
        }
        InkEntry entry;
        for (unsigned c = 0; c < 3; ++c) {
            const Json& v = item[0][c];
            if (!v.is_number_integer() || v.get<std::int64_t>() < 0 || v.get<std::int64_t>() > 255) bad("an ink's RGB is three integers 0..255");
            entry.rgb[c] = static_cast<int>(v.get<std::int64_t>());
        }
        for (const Json& v : item[1]) {
            const auto d = number(v);
            if (!d) bad("an ink value is not a finite number");
            entry.ink.push_back(*d);
        }
        check_ink(out.space, entry.ink);
        if (!seen.insert(entry.rgb).second) bad("an RGB colour is in the table twice");
        out.inks.push_back(std::move(entry));
    }
    return out;
}

const InkEntry* ink_for(const PageColor& color, const std::array<int, 3>& rgb) {
    for (const InkEntry& entry : color.inks) {
        if (entry.rgb == rgb) return &entry;
    }
    return nullptr;
}

std::string bytes_ref(std::string_view bytes) {
    return "sha256:" + QCryptographicHash::hash(QByteArrayView(bytes.data(), static_cast<qsizetype>(bytes.size())),
                                                QCryptographicHash::Sha256).toHex().toStdString();
}

bool lab_blend(std::string_view mode) {
    static constexpr std::array<std::string_view, 9> kModes{"",         "normal",      "multiply",   "screen",    "overlay",
                                                            "add",      "linear_burn", "soft_light", "hard_light"};
    return std::find(kModes.begin(), kModes.end(), mode) != kModes.end();
}

void validate_ink_document(const Document& doc) {
    for (const auto& page : doc.pages) {
        if (std::find(doc.deferred.begin(), doc.deferred.end(), page) != doc.deferred.end()) continue;
        for (const Layer& layer : page->layers) {
            const int kinds = (layer.raster_png && !layer.raster_png->empty()) + (layer.color_raster != nullptr) + (layer.ink_raster != nullptr);
            if (kinds > 1) throw Error("value", "a layer holds one kind of pixels at most (layer " + layer.id + ")");
            if (page->color && page->color->space == InkSpace::Lab && !lab_blend(layer.blend)) {
                throw Error("value", "a Lab page cannot composite the blend " + py_repr_str(layer.blend) + " (layer " + layer.id + ")");
            }
            if (page->color && ((layer.raster_png && !layer.raster_png->empty()) || layer.color_raster)) {
                // (what drew RGB pixels on it does not know the page's inks yet)
                throw Error("not_yet_ported", "RGB pixels on a CMYK or Lab page are not in this build yet (layer " + layer.id + ")");
            }
            if (!layer.ink_raster) continue;
            if (!page->color) throw Error("value", "an RGB page holds no ink pixels (layer " + layer.id + ")");
            const InkRasterHeader header = ink_raster_header(*layer.ink_raster);
            if (header.space != page->color->space || header.depth != page->color->depth) {
                throw Error("value", "the layer " + layer.id + "'s pixels are not in its page's colour mode");
            }
        }
    }
}

InkPixels blank_ink_pixels(InkSpace space, int depth, std::uint32_t width, std::uint32_t height) {
    InkPixels out;
    out.space = space;
    out.depth = depth;
    out.width = width;
    out.height = height;
    check_size(width, height, out.channels(), depth);
    out.samples.assign(out.pixel_count() * out.channels(), 0);
    if (space == InkSpace::Lab) {  // (white: L 100, a = b = 0, transparent)
        const std::uint16_t mid = depth == 8 ? 128 : 32896;
        for (std::size_t i = 0; i < out.pixel_count(); ++i) {
            out.samples[i * 4] = static_cast<std::uint16_t>(out.max());
            out.samples[i * 4 + 1] = mid;
            out.samples[i * 4 + 2] = mid;
        }
    }
    return out;
}

InkRasterHeader ink_raster_header(std::string_view bytes) {
    if (bytes.size() < kHeader || bytes.substr(0, 4) != "GKIR") bad_raster("it is not GKIR");
    if (static_cast<unsigned char>(bytes[4]) != 1) bad_raster("its version is not 1");
    InkRasterHeader out;
    const unsigned space = static_cast<unsigned char>(bytes[5]);
    if (space != 1 && space != 2) bad_raster("its colour space is not CMYK or Lab");
    out.space = space == 1 ? InkSpace::Cmyk : InkSpace::Lab;
    const unsigned depth = static_cast<unsigned char>(bytes[6]);
    if (depth != 8 && depth != 16) bad_raster("its depth is not 8 or 16");
    out.depth = static_cast<int>(depth);
    if (bytes[7] != 0) bad_raster("its reserved byte is not 0");
    out.width = le32(bytes, 8);
    out.height = le32(bytes, 12);
    check_size(out.width, out.height, colour_channels(out.space) + 1, out.depth);
    return out;
}

InkPixels decode_ink_raster(std::string_view bytes) {
    const InkRasterHeader header = ink_raster_header(bytes);
    InkPixels out;
    out.space = header.space;
    out.depth = header.depth;
    out.width = header.width;
    out.height = header.height;
    const unsigned channels = out.channels();
    const std::size_t rb = row_bytes(out.width, channels, out.depth);
    const std::size_t bpp = std::size_t(channels) * (out.depth == 16 ? 2 : 1);
    const std::size_t total = (rb + 1) * out.height;
    std::string raw(total, '\0');

    z_stream z{};
    if (inflateInit(&z) != Z_OK) bad_raster("zlib cannot start");
    const std::string_view stream = bytes.substr(kHeader);
    z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(stream.data()));
    z.next_out = reinterpret_cast<Bytef*>(raw.data());
    std::size_t in_left = stream.size(), out_left = total;
    int status = Z_OK;
    while (true) {
        const uInt in_chunk = static_cast<uInt>(std::min<std::size_t>(in_left, 1u << 30));
        const uInt out_chunk = static_cast<uInt>(std::min<std::size_t>(out_left, 1u << 30));
        z.avail_in = in_chunk;
        z.avail_out = out_chunk;
        status = inflate(&z, Z_NO_FLUSH);
        const std::size_t took = in_chunk - z.avail_in, gave = out_chunk - z.avail_out;
        in_left -= took;
        out_left -= gave;
        if (status == Z_STREAM_END) break;
        if (status != Z_OK && status != Z_BUF_ERROR) break;
        if (took == 0 && gave == 0) break;  // (no progress: input or room ran out)
    }
    inflateEnd(&z);
    if (status != Z_STREAM_END) bad_raster(out_left == 0 ? "it holds more pixels than its size" : "its pixels are cut short or broken");
    if (out_left != 0) bad_raster("it holds fewer pixels than its size");
    if (in_left != 0) bad_raster("bytes follow its pixels");

    auto* data = reinterpret_cast<unsigned char*>(raw.data());
    out.samples.resize(out.pixel_count() * channels);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        unsigned char* line = data + std::size_t(y) * (rb + 1);
        const int type = line[0];
        if (type > 4) bad_raster("a row has an unknown filter");
        const unsigned char* above = y > 0 ? data + std::size_t(y - 1) * (rb + 1) + 1 : nullptr;
        unfilter_row(type, line + 1, above, rb, bpp);
        std::uint16_t* samples = out.samples.data() + std::size_t(y) * out.width * channels;
        if (out.depth == 8) {
            for (std::size_t i = 0; i < rb; ++i) samples[i] = line[1 + i];
        } else {
            for (std::size_t i = 0; i < rb / 2; ++i) samples[i] = static_cast<std::uint16_t>((line[1 + 2 * i] << 8) | line[2 + 2 * i]);
        }
    }
    return out;
}

std::string encode_ink_raster(const InkPixels& pixels) {
    const unsigned channels = pixels.channels();
    check_size(pixels.width, pixels.height, channels, pixels.depth);
    if (pixels.depth != 8 && pixels.depth != 16) bad_raster("its depth is not 8 or 16");
    if (pixels.samples.size() != pixels.pixel_count() * channels) bad_raster("its samples do not fill its size");
    const std::uint32_t most = pixels.max();
    std::string out;
    out.append("GKIR");
    out.push_back(1);
    out.push_back(pixels.space == InkSpace::Cmyk ? 1 : 2);
    out.push_back(static_cast<char>(pixels.depth));
    out.push_back(0);
    put_le32(out, pixels.width);
    put_le32(out, pixels.height);

    const std::size_t rb = row_bytes(pixels.width, channels, pixels.depth);
    const std::size_t bpp = std::size_t(channels) * (pixels.depth == 16 ? 2 : 1);
    std::vector<unsigned char> row(rb), above(rb), best(rb + 1), trial(rb);
    z_stream z{};
    if (deflateInit(&z, 6) != Z_OK) bad_raster("zlib cannot start");
    std::vector<unsigned char> buffer(1 << 16);
    const auto pump = [&](const unsigned char* data, std::size_t n, int flush) {
        z.next_in = const_cast<Bytef*>(data);
        z.avail_in = static_cast<uInt>(n);
        do {
            z.next_out = buffer.data();
            z.avail_out = static_cast<uInt>(buffer.size());
            const int status = deflate(&z, flush);
            if (status == Z_STREAM_ERROR) {
                deflateEnd(&z);
                bad_raster("zlib cannot compress it");
            }
            out.append(reinterpret_cast<const char*>(buffer.data()), buffer.size() - z.avail_out);
        } while (z.avail_out == 0 || (flush == Z_FINISH && z.avail_in != 0));
    };
    for (std::uint32_t y = 0; y < pixels.height; ++y) {
        const std::uint16_t* samples = pixels.samples.data() + std::size_t(y) * pixels.width * channels;
        if (pixels.depth == 8) {
            for (std::size_t i = 0; i < rb; ++i) row[i] = static_cast<unsigned char>(std::min<std::uint32_t>(samples[i], most));
        } else {
            for (std::size_t i = 0; i < rb / 2; ++i) {
                row[2 * i] = static_cast<unsigned char>(samples[i] >> 8);
                row[2 * i + 1] = static_cast<unsigned char>(samples[i] & 255);
            }
        }
        // (libpng's choice: the filter whose bytes, read as signed, sum to the least)
        std::uint64_t least = UINT64_MAX;
        for (int type = 0; type <= 4; ++type) {
            filter_row(type, row.data(), y > 0 ? above.data() : nullptr, rb, bpp, trial.data());
            std::uint64_t sum = 0;
            for (const unsigned char b : trial) sum += b < 128 ? b : 256 - b;
            if (sum < least) {
                least = sum;
                best[0] = static_cast<unsigned char>(type);
                std::memcpy(best.data() + 1, trial.data(), rb);
            }
        }
        pump(best.data(), rb + 1, Z_NO_FLUSH);
        std::swap(row, above);
    }
    pump(nullptr, 0, Z_FINISH);
    deflateEnd(&z);
    return out;
}

}  // namespace genko::core
