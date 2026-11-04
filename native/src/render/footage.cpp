// PC5 J-e2 (video@1): a video layer's footage read and laid on its page (render/footage.hpp). The moving pictures' frames
// as PhotoCraft v0.2.0's crates/engine/src/video_cmds.rs load_frames reads a picture or a folder of them (MIT,
// native/third_party/photocraft/NOTICE.md), widened to every frame of a GIF, an APNG and an animated WebP and to the
// codecs' formats; the frames kept as the book's pictures (Genko's own: the upstream holds them in memory).

#include "render/footage.hpp"

#include <webp/demux.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "core/anim.hpp"
#include "core/color_model.hpp"
#include "core/geometry.hpp"
#include "core/ids.hpp"
#include "core/ink.hpp"
#include "core/pyops.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/smart.hpp"
#include "core/video.hpp"
#include "render/codecs/codecs.hpp"
#include "render/ops_smart_parts.hpp"
#include "render/smart_decode.hpp"
#include "storage/asset_store.hpp"
#include "storage/fsutil.hpp"

namespace genko::render::footage {

namespace fs = std::filesystem;
using core::Json;
using codecs::CodecImage;
using codecs::Layout;
using codecs::Sample;

namespace {

[[noreturn]] void broken(const std::string& what) { throw core::Error("format", what); }

// --- the files ---------------------------------------------------------------------------------------------------------

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool picture_suffix(const fs::path& path) {
    static const std::set<std::string> kSuffixes{".png", ".apng", ".jpg", ".jpeg", ".tif", ".tiff", ".webp", ".bmp", ".dib", ".gif",
                                                 ".tga", ".ico", ".pnm", ".ppm", ".pgm", ".pbm", ".qoi", ".exr", ".hdr", ".avif"};
    return kSuffixes.contains(lower(core::path_to_utf8(path.extension())));
}

bool video_suffix(const fs::path& path) {
    static const std::set<std::string> kVideo{".mp4", ".m4v", ".mov", ".webm", ".mkv", ".avi", ".mpg", ".mpeg", ".wmv", ".flv"};
    return kVideo.contains(lower(core::path_to_utf8(path.extension())));
}

// Names in order, a run of digits as its number ("f9" before "f10").
bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t i2 = i, j2 = j;
            while (i2 < a.size() && a[i2] == '0') ++i2;
            while (j2 < b.size() && b[j2] == '0') ++j2;
            std::size_t ie = i2, je = j2;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ++ie;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) ++je;
            if (ie - i2 != je - j2) return ie - i2 < je - j2;
            const int c = a.compare(i2, ie - i2, b, j2, je - j2);
            if (c != 0) return c < 0;
            i = ie;
            j = je;
            continue;
        }
        if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]);
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

// A stem's name without the digits it ends in (none: it ends in none).
std::optional<std::string> numbered_stem(const std::string& stem) {
    std::size_t end = stem.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(stem[end - 1]))) --end;
    if (end == stem.size()) return std::nullopt;
    return stem.substr(0, end);
}

std::string read_whole(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw core::Error("io", "cannot read the file: " + core::path_to_utf8(path));
    return storage::read_file_bounded(path, storage::kAssetMaxBytes);
}

// --- a moving picture's frames ------------------------------------------------------------------------------------------

struct Moving {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int64_t frames = 0;
};
// Each frame of the canvas (RGBA, 8 bits, straight) and how long it shows (ms).
using EachFrame = std::function<void(const std::string& rgba, int ms)>;

std::uint32_t le16(std::string_view b, std::size_t at) {
    if (at + 2 > b.size()) broken("the GIF is cut short");
    return static_cast<unsigned char>(b[at]) | (static_cast<std::uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8);
}

std::uint32_t be32(std::string_view b, std::size_t at) {
    if (at + 4 > b.size()) broken("the PNG is cut short");
    std::uint32_t v = 0;
    for (int k = 0; k < 4; ++k) v = (v << 8) | static_cast<unsigned char>(b[at + k]);
    return v;
}

// GIF's LZW (codes of min_bits + 1 bits and up, least significant bit first) into at most `count` indices; a stream cut
// short gives what it holds (the rest of the frame not drawn), a code it cannot hold is refused.
std::vector<std::uint8_t> gif_unlzw(std::string_view data, int min_bits, std::size_t count) {
    std::vector<std::uint8_t> out;
    out.reserve(count);
    const int clear = 1 << min_bits, end = clear + 1;
    std::vector<std::uint16_t> prefix(4096, 0);
    std::vector<std::uint8_t> suffix(4096, 0), first(4096, 0);
    std::vector<std::uint16_t> length(4096, 0);
    for (int c = 0; c < clear; ++c) {
        suffix[c] = static_cast<std::uint8_t>(c);
        first[c] = static_cast<std::uint8_t>(c);
        length[c] = 1;
    }
    int size = min_bits + 1, next = end + 1, prev = -1;
    std::uint32_t bits = 0;
    int held = 0;
    std::size_t at = 0;
    std::vector<std::uint8_t> stack(4096);
    const auto emit = [&](int code) {
        int n = length[code];
        for (int k = n - 1, c = code; k >= 0; --k) {
            stack[k] = suffix[c];
            c = prefix[c];
        }
        for (int k = 0; k < n && out.size() < count; ++k) out.push_back(stack[k]);
    };
    while (out.size() < count) {
        while (held < size && at < data.size()) {
            bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[at++])) << held;
            held += 8;
        }
        if (held < size) break;  // (cut short: what it held)
        const int code = static_cast<int>(bits & ((1u << size) - 1));
        bits >>= size;
        held -= size;
        if (code == clear) {
            size = min_bits + 1;
            next = end + 1;
            prev = -1;
            continue;
        }
        if (code == end) break;
        if (prev < 0) {
            if (code >= clear) broken("the GIF's picture data is broken");
            emit(code);
            prev = code;
            continue;
        }
        std::uint8_t lead = 0;
        if (code < next) {
            lead = first[code];
            emit(code);
        } else if (code == next && next < 4096) {
            lead = first[prev];
        } else {
            broken("the GIF's picture data is broken");
        }
        if (next < 4096) {
            prefix[next] = static_cast<std::uint16_t>(prev);
            suffix[next] = lead;
            first[next] = first[prev];
            length[next] = static_cast<std::uint16_t>(std::min<int>(4095, length[prev] + 1));
            if (code == next) emit(next);
            ++next;
            if (next == (1 << size) && size < 12) ++size;
        }
        prev = code;
    }
    return out;
}

// The GIF's canvas and frames (each drawn over what the one before left by its disposal: kept, cleared, or put back).
// `each` null: only counted.
std::optional<Moving> gif_frames(std::string_view b, const codecs::Limits& limits, const EachFrame* each) {
    if (b.size() < 13 || (b.substr(0, 6) != "GIF87a" && b.substr(0, 6) != "GIF89a")) return std::nullopt;
    Moving m;
    m.width = le16(b, 6);
    m.height = le16(b, 8);
    limits.check(m.width, m.height, Layout::Rgba, Sample::U8);
    const std::uint32_t flags = static_cast<unsigned char>(b[10]);
    std::size_t pos = 13;
    std::string_view global;
    if (flags & 0x80U) {
        const std::size_t bytes = 3u << ((flags & 7U) + 1U);
        if (pos + bytes > b.size()) broken("the GIF is cut short");
        global = b.substr(pos, bytes);
        pos += bytes;
    }
    const std::size_t w = m.width, h = m.height;
    std::string canvas, saved;
    if (each != nullptr) canvas.assign(w * h * 4, '\0');
    int disposal = 0, transparent = -1, delay = 0;
    const auto blocks = [&](std::string* into) {
        for (;;) {
            if (pos >= b.size()) broken("the GIF is cut short");
            const std::size_t n = static_cast<unsigned char>(b[pos++]);
            if (n == 0) return;
            if (pos + n > b.size()) broken("the GIF is cut short");
            if (into != nullptr) into->append(b.substr(pos, n));
            pos += n;
        }
    };
    while (pos < b.size()) {
        const unsigned char marker = static_cast<unsigned char>(b[pos++]);
        if (marker == 0x3B) break;
        if (marker == 0x00) continue;  // (padding, as the readers skip it)
        if (marker == 0x21) {
            if (pos >= b.size()) broken("the GIF is cut short");
            const unsigned char label = static_cast<unsigned char>(b[pos++]);
            std::string data;
            blocks(label == 0xF9 ? &data : nullptr);
            if (label == 0xF9 && data.size() >= 4) {
                const unsigned packed = static_cast<unsigned char>(data[0]);
                disposal = static_cast<int>((packed >> 2) & 7U);
                delay = static_cast<int>(le16(data, 1));
                transparent = (packed & 1U) ? static_cast<unsigned char>(data[3]) : -1;
            }
            continue;
        }
        if (marker != 0x2C) broken("the GIF's blocks are broken");
        if (pos + 9 > b.size()) broken("the GIF is cut short");
        const std::size_t fx = le16(b, pos), fy = le16(b, pos + 2), fw = le16(b, pos + 4), fh = le16(b, pos + 6);
        const unsigned frame_flags = static_cast<unsigned char>(b[pos + 8]);
        pos += 9;
        std::string_view palette = global;
        if (frame_flags & 0x80U) {
            const std::size_t bytes = 3u << ((frame_flags & 7U) + 1U);
            if (pos + bytes > b.size()) broken("the GIF is cut short");
            palette = b.substr(pos, bytes);
            pos += bytes;
        }
        if (pos >= b.size()) broken("the GIF is cut short");
        const int min_bits = static_cast<unsigned char>(b[pos++]);
        if (min_bits < 1 || min_bits > 11) broken("the GIF's code size is broken");
        std::string data;
        blocks(each != nullptr ? &data : nullptr);
        if (++m.frames > core::anim::kMaxFrames * 16) throw core::Error("value", "the moving picture has too many frames");
        if (each != nullptr) {
            if (fw == 0 || fh == 0) broken("the GIF's frame has no size");
            if (static_cast<std::uint64_t>(fw) * fh > static_cast<std::uint64_t>(limits.max_pixels)) {
                throw core::Error("image_too_large", "the GIF's frame is too large");
            }
            if (disposal == 3) saved = canvas;
            const std::vector<std::uint8_t> indices = gif_unlzw(data, min_bits, fw * fh);
            const bool interlaced = (frame_flags & 0x40U) != 0;
            std::vector<std::size_t> rows(fh);
            if (interlaced) {
                std::size_t r = 0;
                for (const auto& [start, step] : {std::pair<std::size_t, std::size_t>{0, 8}, {4, 8}, {2, 4}, {1, 2}}) {
                    for (std::size_t y = start; y < fh; y += step) rows[r++] = y;
                }
            } else {
                for (std::size_t y = 0; y < fh; ++y) rows[y] = y;
            }
            for (std::size_t i = 0; i < indices.size(); ++i) {
                const std::size_t x = fx + i % fw, y = fy + rows[i / fw];
                const int index = indices[i];
                if (x >= w || y >= h || index == transparent) continue;
                char* px = canvas.data() + (y * w + x) * 4;
                if (static_cast<std::size_t>(index) * 3 + 2 < palette.size()) {
                    px[0] = palette[index * 3];
                    px[1] = palette[index * 3 + 1];
                    px[2] = palette[index * 3 + 2];
                } else {
                    px[0] = px[1] = px[2] = 0;
                }
                px[3] = static_cast<char>(0xFF);
            }
            (*each)(canvas, delay <= 1 ? 100 : delay * 10);
            if (disposal == 2) {
                for (std::size_t y = fy; y < std::min(h, fy + fh); ++y) {
                    for (std::size_t x = fx; x < std::min(w, fx + fw); ++x) std::fill_n(canvas.data() + (y * w + x) * 4, 4, '\0');
                }
            } else if (disposal == 3) {
                canvas = saved;
            }
        }
        disposal = 0;
        transparent = -1;
        delay = 0;
    }
    if (m.frames == 0) broken("the GIF has no picture");
    return m;
}

void put_chunk(std::string& out, std::string_view type, std::string_view data) {
    const auto put32 = [&](std::uint32_t v) {
        for (int k = 3; k >= 0; --k) out.push_back(static_cast<char>((v >> (k * 8)) & 0xFF));
    };
    put32(static_cast<std::uint32_t>(data.size()));
    std::string body(type);
    body.append(data);
    out.append(body);
    put32(static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
}

// An APNG's canvas and frames (fcTL: each frame's place, delay, disposal and blending; IDAT and fdAT its data): each
// frame a PNG of its own made of the file's header chunks and read by the codecs. None: a PNG of one picture.
std::optional<Moving> apng_frames(std::string_view b, const codecs::Limits& limits, const EachFrame* each) {
    static constexpr std::string_view kSignature("\x89PNG\r\n\x1a\n", 8);
    if (b.size() < 8 || b.substr(0, 8) != kSignature) return std::nullopt;
    struct Frame {
        std::uint32_t w = 0, h = 0, x = 0, y = 0;
        int ms = 100;
        int dispose = 0, blend = 0;
        std::string data;
    };
    std::string ihdr;
    std::string header_chunks;  // (PLTE, tRNS, gAMA… before the first picture data)
    bool animated = false, seen_data = false;
    std::vector<Frame> frames;
    std::size_t pos = 8;
    while (pos + 8 <= b.size()) {
        const std::uint32_t length = be32(b, pos);
        const std::string_view type = b.substr(pos + 4, 4);
        if (length > b.size() || pos + 12 + length > b.size()) broken("the PNG is cut short");
        const std::string_view data = b.substr(pos + 8, length);
        pos += 12 + length;
        if (type == "IHDR") {
            if (length != 13) broken("the PNG's header is broken");
            ihdr = std::string(data);
        } else if (type == "acTL") {
            animated = true;
        } else if (type == "fcTL") {
            if (length != 26) broken("the APNG's frame control is broken");
            Frame f;
            f.w = be32(data, 4);
            f.h = be32(data, 8);
            f.x = be32(data, 12);
            f.y = be32(data, 16);
            const std::uint32_t num = static_cast<unsigned char>(data[20]) << 8 | static_cast<unsigned char>(data[21]);
            std::uint32_t den = static_cast<unsigned char>(data[22]) << 8 | static_cast<unsigned char>(data[23]);
            if (den == 0) den = 100;
            f.ms = static_cast<int>(std::lround(1000.0 * num / den));
            f.dispose = static_cast<unsigned char>(data[24]);
            f.blend = static_cast<unsigned char>(data[25]);
            frames.push_back(std::move(f));
            if (static_cast<std::int64_t>(frames.size()) > core::anim::kMaxFrames * 16) throw core::Error("value", "the moving picture has too many frames");
        } else if (type == "IDAT") {
            seen_data = true;
            if (!frames.empty() && each != nullptr) frames.back().data.append(data);  // (the default picture, a frame when an fcTL is before it)
        } else if (type == "fdAT") {
            if (length < 4 || frames.empty()) broken("the APNG's frame data is broken");
            if (each != nullptr) frames.back().data.append(data.substr(4));
        } else if (type == "IEND") {
            break;
        } else if (!seen_data && type != "IHDR") {
            header_chunks.append(b.substr(pos - 12 - length, 12 + length));
        }
    }
    if (!animated || frames.empty()) return std::nullopt;
    if (ihdr.empty()) broken("the PNG has no header");
    Moving m;
    m.width = be32(ihdr, 0);
    m.height = be32(ihdr, 4);
    limits.check(m.width, m.height, Layout::Rgba, Sample::U8);
    m.frames = static_cast<std::int64_t>(frames.size());
    if (each == nullptr) return m;
    const std::size_t w = m.width, h = m.height;
    std::string canvas(w * h * 4, '\0'), saved;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        Frame& f = frames[i];
        if (f.w == 0 || f.h == 0 || f.x > m.width || f.y > m.height || f.w > m.width - f.x || f.h > m.height - f.y) {
            broken("the APNG's frame is outside its picture");
        }
        if (f.data.empty()) broken("the APNG's frame has no picture data");
        std::string png(kSignature);
        std::string header = ihdr;
        for (int k = 0; k < 4; ++k) {
            header[k] = static_cast<char>((f.w >> (24 - 8 * k)) & 0xFF);
            header[4 + k] = static_cast<char>((f.h >> (24 - 8 * k)) & 0xFF);
        }
        put_chunk(png, "IHDR", header);
        png.append(header_chunks);
        put_chunk(png, "IDAT", f.data);
        put_chunk(png, "IEND", "");
        std::string().swap(f.data);
        const CodecImage part = codecs::decode_as(codecs::Format::Png, png, limits).convert(Layout::Rgba, Sample::U8);
        const int dispose = i == 0 && f.dispose == 2 ? 1 : f.dispose;
        if (dispose == 2) saved = canvas;
        const auto* src = reinterpret_cast<const unsigned char*>(part.data().data());
        for (std::size_t y = 0; y < f.h; ++y) {
            for (std::size_t x = 0; x < f.w; ++x) {
                const unsigned char* s = src + (y * f.w + x) * 4;
                auto* d = reinterpret_cast<unsigned char*>(canvas.data() + ((f.y + y) * w + f.x + x) * 4);
                if (f.blend == 0 || s[3] == 255) {
                    std::copy_n(s, 4, d);
                } else if (s[3] != 0) {  // (over: straight alpha)
                    const double sa = s[3] / 255.0, da = d[3] / 255.0, oa = sa + da * (1 - sa);
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<unsigned char>(std::lround((s[c] * sa + d[c] * da * (1 - sa)) / oa));
                    d[3] = static_cast<unsigned char>(std::lround(oa * 255));
                }
            }
        }
        (*each)(canvas, f.ms <= 0 ? 100 : f.ms);
        if (dispose == 1) {
            for (std::size_t y = f.y; y < f.y + f.h; ++y) std::fill_n(canvas.data() + (y * w + f.x) * 4, f.w * 4, '\0');
        } else if (dispose == 2) {
            canvas = saved;
        }
    }
    return m;
}

struct WebPDecoder {
    WebPAnimDecoder* decoder = nullptr;
    ~WebPDecoder() {
        if (decoder != nullptr) WebPAnimDecoderDelete(decoder);
    }
};

// An animated WebP's canvas and frames (libwebp's animation decoder: each frame the canvas as it shows). None: a WebP of
// one picture.
std::optional<Moving> webp_frames(std::string_view b, const codecs::Limits& limits, const EachFrame* each) {
    if (b.size() < 16 || b.substr(0, 4) != "RIFF" || b.substr(8, 4) != "WEBP") return std::nullopt;
    WebPData data{reinterpret_cast<const uint8_t*>(b.data()), b.size()};
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) broken("the WebP cannot be read");
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;
    WebPDecoder anim;
    anim.decoder = WebPAnimDecoderNew(&data, &options);
    if (anim.decoder == nullptr) broken("the WebP cannot be read");
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(anim.decoder, &info)) broken("the WebP cannot be read");
    if (info.frame_count <= 1) return std::nullopt;
    Moving m;
    m.width = info.canvas_width;
    m.height = info.canvas_height;
    limits.check(m.width, m.height, Layout::Rgba, Sample::U8);
    m.frames = info.frame_count;
    if (each == nullptr) return m;
    int before = 0;
    while (WebPAnimDecoderHasMoreFrames(anim.decoder)) {
        uint8_t* frame = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(anim.decoder, &frame, &timestamp) || frame == nullptr) broken("the WebP's frame cannot be read");
        (*each)(std::string(reinterpret_cast<const char*>(frame), static_cast<std::size_t>(m.width) * m.height * 4),
                timestamp - before <= 0 ? 100 : timestamp - before);
        before = timestamp;
    }
    return m;
}

std::optional<Moving> moving(std::string_view bytes, const codecs::Limits& limits, const EachFrame* each) {
    if (auto m = gif_frames(bytes, limits, each)) return m->frames > 1 ? m : std::nullopt;
    if (auto m = apng_frames(bytes, limits, each)) return m->frames > 1 ? m : std::nullopt;
    return webp_frames(bytes, limits, each);
}

// --- frames into PNGs ---------------------------------------------------------------------------------------------------

std::string png_of(const CodecImage& rgba) {
    CodecImage plain = rgba;
    plain.icc.reset();
    plain.meta = {};
    codecs::EncodeOptions options;
    options.embed_icc = false;
    options.embed_metadata = false;
    return codecs::encode(plain, codecs::Format::Png, options);
}

// A picture read for a sequence: RGBA of 8 bits, or of 16 where it holds more (its colours as numbers; linear light given
// sRGB's curve).
CodecImage normal_picture(std::string_view bytes, const codecs::Limits& limits, std::vector<std::string>& notes, bool deep) {
    CodecImage picture = codecs::decode(bytes, limits);
    if (const auto format = codecs::detect(bytes); format && codecs::linear_light(*format)) picture = codecs::srgb_encoded(picture);
    if (picture.icc && std::find(notes.begin(), notes.end(), "素材の色のプロファイルは使わず、数値のまま sRGB として扱います") == notes.end()) {
        notes.emplace_back("素材の色のプロファイルは使わず、数値のまま sRGB として扱います");
    }
    if (codecs::is_cmyk(picture.layout()) &&
        std::find(notes.begin(), notes.end(), "CMYK の素材は色の管理をせずに RGB にしました") == notes.end()) {
        notes.emplace_back("CMYK の素材は色の管理をせずに RGB にしました");
    }
    return picture.convert(Layout::Rgba, deep ? Sample::U16 : Sample::U8);
}


void check_room(std::uint64_t width, std::uint64_t height, std::uint64_t frames, int depth, std::uint64_t most) {
    const std::uint64_t need = width * height * 4 * (depth > 8 ? 2 : 1) * frames;
    if (need > most) {
        throw core::Error("footage_too_large", "the footage would take " + std::to_string(need / (1024 * 1024)) + " MiB decoded; the book's footage may take " +
                                                   std::to_string(most / (1024 * 1024)) + " MiB more (read part of it: range, every)");
    }
}

// The frames chosen of a file's `count`: [first, last] every n-th.
std::vector<std::int64_t> chosen(std::int64_t count, const ReadOptions& options) {
    if (options.every < 1 || options.every > core::anim::kMaxFrames) throw core::Error("value", "every is 1 to 3000");
    std::int64_t first = 1, last = count;
    if (options.range) {
        first = (*options.range)[0];
        last = (*options.range)[1];
        if (first < 1 || last > count || first > last) {
            throw core::Error("value", "range is [first, last] of the frames 1 to " + std::to_string(count));
        }
    }
    std::vector<std::int64_t> out;
    for (std::int64_t f = first; f <= last; f += options.every) out.push_back(f);
    if (static_cast<std::int64_t>(out.size()) > core::anim::kMaxFrames) {
        throw core::Error("value", "the footage has more than 3000 frames (read part of it: range, every)");
    }
    return out;
}

Json footage_json(const std::string& kind, const std::string& name, const Json& frames, double fps, std::uint32_t w, std::uint32_t h, int depth,
                  const Json& source) {
    Json out = Json::object();
    out["kind"] = kind;
    out["name"] = name.size() > 1024 ? name.substr(0, 1024) : name;
    out["frames"] = frames;
    out["fps"] = fps;
    out["size_px"] = Json::array({w, h});
    out["depth"] = depth;
    out["alpha"] = "straight";
    out["par"] = 1.0;
    out["source"] = source;
    return out;
}

}  // namespace

std::vector<fs::path> sequence_files(const fs::path& path, bool sequence) {
    std::error_code ec;
    std::vector<fs::path> out;
    if (fs::is_directory(path, ec)) {
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (entry.is_regular_file(ec) && picture_suffix(entry.path())) out.push_back(entry.path());
        }
    } else {
        const auto stem = numbered_stem(core::path_to_utf8(path.stem()));
        const fs::path folder = path.parent_path().empty() ? fs::path(".") : path.parent_path();
        if (sequence && stem && fs::is_directory(folder, ec)) {
            const std::string suffix = lower(core::path_to_utf8(path.extension()));
            for (const auto& entry : fs::directory_iterator(folder, ec)) {
                if (!entry.is_regular_file(ec) || lower(core::path_to_utf8(entry.path().extension())) != suffix) continue;
                const std::string other = core::path_to_utf8(entry.path().stem());
                const auto other_stem = numbered_stem(other);
                if (other_stem && *other_stem == *stem) out.push_back(entry.path());
            }
        }
        if (out.empty()) out.push_back(path);
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        return natural_less(core::path_to_utf8(a.filename()), core::path_to_utf8(b.filename()));
    });
    return out;
}

Footage read_footage(const fs::path& path, const ReadOptions& options, const Store& store) {
    if (options.fps && !(*options.fps >= core::kTimelineFpsMin && *options.fps <= core::kTimelineFpsMax)) {
        throw core::Error("value", "fps is 0.5 to 120");
    }
    std::error_code ec;
    if (!fs::exists(path, ec)) throw core::Error("io", "no such file or folder: " + core::path_to_utf8(path));
    if (video_suffix(path)) {
        throw core::Error("video_file", "a video file (MP4, MOV, WebM…) is not read: a numbered sequence of pictures, a GIF, an APNG or a WebP is");
    }
    Footage out;
    const std::vector<fs::path> files = sequence_files(path, options.sequence);
    if (files.empty()) throw core::Error("value", "no pictures in the folder: " + core::path_to_utf8(path));
    // (one file: a moving picture's frames, or one picture)
    if (files.size() == 1 && !fs::is_directory(path, ec)) {
        const std::string bytes = read_whole(files.front());
        const std::string sha = storage::sha256_hex(bytes);
        const Json source = Json::object({{"path", core::path_to_utf8(fs::absolute(files.front(), ec))}, {"bytes", bytes.size()}, {"sha256", sha}});
        const std::string name = core::path_to_utf8(files.front().filename());
        if (const auto m = moving(bytes, options.limits, nullptr)) {
            const std::vector<std::int64_t> picked = chosen(m->frames, options);
            check_room(m->width, m->height, picked.size(), 8, options.most_bytes);
            std::vector<std::pair<std::string, int>> frames;  // (ref, ms)
            std::size_t next = 0;
            std::int64_t at = 0;
            const EachFrame keep = [&](const std::string& rgba, int ms) {
                ++at;
                if (next >= picked.size() || picked[next] != at) return;
                ++next;
                const CodecImage picture = CodecImage::from_bytes(m->width, m->height, Layout::Rgba, Sample::U8, rgba);
                frames.emplace_back(store(png_of(picture)), ms * static_cast<int>(options.every));
            };
            (void)moving(bytes, options.limits, &keep);
            if (frames.size() != picked.size()) broken("the moving picture's frames cannot all be read");
            // (laid at its shortest frame's speed, a longer one shown as many times as it lasts)
            int shortest = frames.front().second;
            for (const auto& [ref, ms] : frames) shortest = std::min(shortest, ms);
            double fps = options.fps.value_or(std::clamp(1000.0 / std::max(1, shortest), core::kTimelineFpsMin, core::kTimelineFpsMax));
            const double each_ms = 1000.0 / fps;
            Json refs = Json::array();
            for (const auto& [ref, ms] : frames) {
                const auto times = options.fps ? 1 : std::max<std::int64_t>(1, std::llround(ms / each_ms));
                for (std::int64_t k = 0; k < times; ++k) refs.push_back(ref);
                if (static_cast<std::int64_t>(refs.size()) > core::anim::kMaxFrames) {
                    throw core::Error("value", "the footage has more than 3000 frames (read part of it: range, every)");
                }
            }
            fps = std::round(fps * 1000) / 1000;  // (3 places, as the timeline's field shows it)
            out.read = static_cast<std::int64_t>(frames.size());
            out.json = footage_json("animation", name, refs, fps, m->width, m->height, 8, source);
            return out;
        }
        (void)chosen(1, options);
        const CodecImage first = codecs::decode(bytes, options.limits);
        const bool deep = first.sample() != Sample::U8;
        check_room(first.width(), first.height(), 1, deep ? 16 : 8, options.most_bytes);
        const CodecImage picture = normal_picture(bytes, options.limits, out.notes, deep);
        out.read = 1;
        out.json = footage_json("image", name, Json::array({store(png_of(picture))}), options.fps.value_or(30.0), picture.width(), picture.height(),
                                deep ? 16 : 8, source);
        return out;
    }
    // (a numbered sequence)
    const std::vector<std::int64_t> picked = chosen(static_cast<std::int64_t>(files.size()), options);
    const std::string first_bytes = read_whole(files[static_cast<std::size_t>(picked.front() - 1)]);
    const auto size = codecs::header_size(first_bytes);
    if (!size) throw core::Error("unidentified_image", "cannot identify image file: " + core::path_to_utf8(files[static_cast<std::size_t>(picked.front() - 1)]));
    check_room(size->first, size->second, picked.size(), 8, options.most_bytes);
    Json refs = Json::array();
    std::string listed;
    std::uint64_t total = 0;
    bool deep = false;
    std::uint32_t width = 0, height = 0;
    for (std::size_t i = 0; i < picked.size(); ++i) {
        const fs::path& file = files[static_cast<std::size_t>(picked[i] - 1)];
        const std::string bytes = i == 0 ? first_bytes : read_whole(file);
        total += bytes.size();
        listed += storage::sha256_hex(bytes) + "\n";
        if (i == 0) {
            const CodecImage probe = codecs::decode(bytes, options.limits);
            deep = probe.sample() != Sample::U8;
            width = probe.width();
            height = probe.height();
            check_room(width, height, picked.size(), deep ? 16 : 8, options.most_bytes);
        }
        const CodecImage picture = normal_picture(bytes, options.limits, out.notes, deep);
        if (picture.width() != width || picture.height() != height) {
            throw core::Error("value", "the sequence's pictures are not all of one size (" + std::to_string(width) + "×" + std::to_string(height) + ", " +
                                           core::path_to_utf8(file.filename()) + " " + std::to_string(picture.width()) + "×" +
                                           std::to_string(picture.height()) + ")");
        }
        refs.push_back(store(png_of(picture)));
    }
    const bool folder = fs::is_directory(path, ec);
    const fs::path named = folder ? path : files.front();
    const Json source = Json::object({{"path", core::path_to_utf8(fs::absolute(named, ec))}, {"bytes", total}, {"sha256", storage::sha256_hex(listed)}});
    std::string name = core::path_to_utf8((folder ? path : files.front()).filename());
    if (folder && name.empty()) name = core::path_to_utf8(path.parent_path().filename());
    out.read = static_cast<std::int64_t>(picked.size());
    out.json = footage_json(picked.size() == 1 && !folder ? "image" : "sequence", name, refs, options.fps.value_or(30.0), width, height, deep ? 16 : 8, source);
    return out;
}

Store book_store(const fs::path& book) {
    return [book](const std::string& png) { return storage::AssetStore(book).put_bytes(png, core::kFootageSuffix); };
}

Store memory_store(core::Document& doc) {
    return [&doc](const std::string& png) {
        std::string ref = storage::AssetStore::ref(png);
        if (!doc.smart_files.contains(ref)) doc.smart_files[ref] = std::make_shared<const std::string>(png);
        return ref;
    };
}

std::optional<std::string> frame_bytes(const core::Document& doc, const std::string& ref) {
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) return *held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(ref)) return std::nullopt;
    const storage::AssetStore store(*doc.asset_dir);
    if (!store.has(ref, core::kFootageSuffix)) return std::nullopt;
    auto bytes = store.get_bytes(ref, core::kFootageSuffix, storage::kAssetMaxBytes);
    if (bytes && storage::AssetStore::ref(*bytes) != ref) return std::nullopt;  // (not the bytes its name says)
    return bytes;
}

// --- a footage frame laid on its page -----------------------------------------------------------------------------------

namespace {

struct Made {
    core::Bytes raster_png, color_raster, ink_raster;
    std::optional<Json> screen;
    std::int64_t bytes = 0;
};

struct Cache {
    std::mutex mutex;
    std::list<std::pair<std::string, std::shared_ptr<const Made>>> order;  // (latest first)
    std::unordered_map<std::string, decltype(order)::iterator> by_key;
    std::int64_t bytes = 0;
    std::uint64_t made = 0;
};
Cache& cache() {
    static Cache c;
    return c;
}
constexpr std::int64_t kCacheBytes = 256ll << 20;

std::int64_t size_of(const core::Bytes& b) { return b ? static_cast<std::int64_t>(b->size()) : 0; }

// The frame's picture as its alpha is read: straight as it is, premultiplied made straight, ignored made opaque.
std::string with_alpha(const std::string& png, const std::string& alpha) {
    if (alpha == "straight") return png;
    CodecImage picture = codecs::decode_as(codecs::Format::Png, png);
    const bool deep = picture.sample() == Sample::U16;
    picture = picture.convert(Layout::Rgba, deep ? Sample::U16 : Sample::U8);
    const std::size_t n = picture.pixel_count();
    const auto fix = [&](auto* px, double top) {
        for (std::size_t i = 0; i < n; ++i) {
            auto* p = px + i * 4;
            if (alpha == "ignore") {
                p[3] = static_cast<std::remove_reference_t<decltype(*p)>>(top);
            } else if (p[3] != 0 && p[3] != top) {
                for (int c = 0; c < 3; ++c) p[c] = static_cast<std::remove_reference_t<decltype(*p)>>(std::min(top, std::round(p[c] * top / p[3])));
            }
        }
    };
    if (deep) fix(reinterpret_cast<std::uint16_t*>(picture.data().data()), 65535.0);
    else fix(reinterpret_cast<unsigned char*>(picture.data().data()), 255.0);
    return png_of(picture);
}

std::shared_ptr<const Made> make(const core::Document& doc, const core::Page& page, const Json& video, std::int64_t index) {
    const Json& footage = video.at("footage");
    const std::string ref = footage.at("frames").at(static_cast<std::size_t>(index)).get<std::string>();
    const std::optional<std::string> held = frame_bytes(doc, ref);
    if (!held) throw core::Error("missing_asset", "the footage's frame " + ref + " is not in the book");
    const std::string alpha = footage.at("alpha").get<std::string>();
    const auto placement = core::footage_placement(video);
    Json key = Json::array({doc.asset_dir ? core::path_to_utf8(*doc.asset_dir) : std::string(), ref, alpha, core::rect_to_json(*placement),
                            page.spec.width_mm.value(), page.spec.height_mm.value(), page.spec.expression,
                            page.color ? core::to_json(*page.color) : Json(), page.rgb ? core::to_json(*page.rgb) : Json(),
                            core::py_get(core::py_or(core::py_get(doc.studio, "style"), Json::object()), "finish")});
    const std::string text = key.dump();
    {
        Cache& c = cache();
        std::lock_guard lock(c.mutex);
        if (const auto found = c.by_key.find(text); found != c.by_key.end()) {
            c.order.splice(c.order.begin(), c.order, found->second);
            return found->second->second;
        }
    }
    // (the scratch book's ids apart from the op's: an op that lays a frame takes the same ids whether it was remembered or not
    // — the journal plays it again)
    const core::ScopedIdsApart apart;
    const std::string bytes = with_alpha(*held, alpha);
    const smart::FileInfo info = smart::look_at(bytes, core::kFootageSuffix);
    core::Document scratch;
    scratch.asset_dir = doc.asset_dir;
    scratch.studio = doc.studio;
    scratch.features = doc.features;
    const std::string asset = storage::AssetStore::ref(bytes);
    scratch.smart_files[asset] = std::make_shared<const std::string>(bytes);
    auto contents = std::make_shared<core::SmartContents>();
    contents->kind = "file";
    contents->name = footage.at("name").get<std::string>();
    contents->asset = asset;
    contents->suffix = std::string(core::kFootageSuffix);
    contents->format = info.format;
    contents->width_px = info.width;
    contents->height_px = info.height;
    contents->dpi = info.dpi > 0 ? info.dpi : 72.0;
    contents->depth = info.depth;
    contents->mode = info.mode;
    const std::string contents_id = smart_ops::fresh_contents_id(scratch);
    scratch.smart_contents[contents_id] = contents;
    const auto [cw, ch] = core::smart::size_mm(*contents);
    const core::Rect& r = *placement;
    core::smart::Matrix m{r.width.value() / cw, 0, 0, r.height.value() / ch, r.x.value(), r.y.value()};
    auto alone = std::make_shared<core::Page>(page);
    alone->layers.clear();
    if (alone->extra.is_object()) alone->extra.erase("anim");
    core::Layer layer;
    layer.id = "footage";
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Smart;
    layer.exportable = true;
    layer.clip_to = "none";
    layer.smart = core::smart::settings(contents_id, m, "lanczos");
    alone->layers.push_back(std::move(layer));
    scratch.pages = {alone};
    smart_ops::rasterize(scratch, 0, 0);
    const core::Layer& done = scratch.page(0).layers[0];
    auto out = std::make_shared<Made>();
    out->raster_png = done.raster_png;
    out->color_raster = done.color_raster;
    out->ink_raster = done.ink_raster;
    out->screen = done.screen;
    out->bytes = size_of(done.raster_png) + size_of(done.color_raster) + size_of(done.ink_raster);
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    ++c.made;
    if (c.by_key.contains(text)) return out;
    c.order.emplace_front(text, out);
    c.by_key[text] = c.order.begin();
    c.bytes += out->bytes;
    while (c.bytes > kCacheBytes && c.order.size() > 1) {
        c.bytes -= c.order.back().second->bytes;
        c.by_key.erase(c.order.back().first);
        c.order.pop_back();
    }
    return out;
}

}  // namespace

core::Layer footage_layer(const core::Document& doc, const core::Page& page, const Json& video, std::int64_t index) {
    const std::shared_ptr<const Made> made = make(doc, page, video, index);
    core::Layer layer;
    layer.role = core::LayerRole::User;
    layer.kind = core::LayerKind::Raster;
    layer.exportable = true;
    layer.raster_png = made->raster_png;
    layer.color_raster = made->color_raster;
    layer.ink_raster = made->ink_raster;
    if (made->screen) layer.screen = *made->screen;
    return layer;
}

namespace {

void lay_footage(core::Page& out, const core::Page& page, std::int64_t frame, const core::Document* episode, std::vector<std::string>* laid) {
    if (episode == nullptr || !core::page_uses_video(page)) return;
    const Json* spec = core::anim::spec(page);
    for (const Json& track : spec->at("tracks")) {
        if (!core::is_video_row(track)) continue;
        const Json& folder = track.at("folder");
        if (!folder.is_string()) continue;
        const Json& video = track.at("video");
        const core::VideoShown shown = core::video_shown(page, folder, video, frame);
        if (shown.kind != core::VideoShown::Kind::Footage) continue;
        const std::string& id = folder.get_ref<const std::string&>();
        const auto at = std::find_if(out.layers.begin(), out.layers.end(), [&](const core::Layer& l) { return l.id == id; });
        if (at == out.layers.end() || at->kind != core::LayerKind::Folder) continue;
        core::Layer layer;
        try {
            layer = footage_layer(*episode, page, video, shown.index);
        } catch (const std::exception&) {  // (a frame not held or not drawn: left out)
            continue;
        }
        layer.id = id + "~footage";
        layer.title = at->title;
        layer.parent_id = id;
        layer.visible = at->visible;
        if (laid != nullptr) laid->push_back(at->title);
        out.layers.insert(at, std::move(layer));  // (a folder's layers sit just under it)
    }
}

}  // namespace

core::Page frame_page(const core::Page& page, std::int64_t frame, const core::Document* episode) {
    core::Page out = core::anim::at_frame(page, frame);
    lay_footage(out, page, frame, episode, nullptr);
    return out;
}

core::Page with_footage(const core::Page& page, std::int64_t frame, const core::Document* episode, std::vector<std::string>* laid) {
    core::Page out = page;
    lay_footage(out, page, frame, episode, laid);
    return out;
}

void clear_cache() {
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    c.order.clear();
    c.by_key.clear();
    c.bytes = 0;
}

std::int64_t cache_bytes() {
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    return c.bytes;
}

std::uint64_t layers_made() {
    Cache& c = cache();
    std::lock_guard lock(c.mutex);
    return c.made;
}

}  // namespace genko::render::footage
