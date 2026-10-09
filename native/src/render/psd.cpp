#include "render/psd.hpp"

#include <zlib.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

#include "render/npcompat.hpp"
#include "render/op_limits.hpp"

namespace genko::render::psd {

ReadError::ReadError(std::string type, const std::string& message, bool caught, bool too_large)
    : core::Error("psd", message), type_(std::move(type)), caught_(caught), too_large_(too_large) {}

Limits default_limits() {
    return Limits{4 * limits::kPixels, limits::kPixels, std::int64_t{1} << 33};
}

const File::ChannelRef* File::Record::channel(int id) const {
    for (const ChannelRef& c : data) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

namespace {

// A position past any file. Python's positions are unbounded ints (a PSB's lengths are 64 bits, added to positions);
// every position at or past the end of the file reads the same (nothing can be taken there), so they are held here.
constexpr std::uint64_t kFar = std::uint64_t{1} << 62;

std::uint64_t past(std::uint64_t pos, std::uint64_t n) { return n >= kFar - std::min(pos, kFar) ? kFar : pos + n; }

std::uint64_t held_product(std::uint64_t a, std::uint64_t b) {
    if (a == 0 || b == 0) return 0;
    return a > kFar / b ? kFar : a * b;
}

[[noreturn]] void psd_error(const std::string& message) { throw ReadError("PSDError", message, true); }
[[noreturn]] void value_error(const std::string& message) { throw ReadError("ValueError", message, true); }
[[noreturn]] void too_large(const std::string& message) { throw ReadError("TooLarge", message, true, true); }

unsigned byte_at(std::string_view d, std::size_t i) { return static_cast<unsigned char>(d[i]); }

std::uint32_t be32(std::string_view d) {
    return (std::uint32_t{byte_at(d, 0)} << 24) | (std::uint32_t{byte_at(d, 1)} << 16) | (std::uint32_t{byte_at(d, 2)} << 8) |
           std::uint32_t{byte_at(d, 3)};
}

// psd._Reader: the bytes from a position, "the PSD file is cut short" where they run out.
class Reader {
public:
    Reader(std::string_view data, bool psb) : data_(data), psb_(psb) {}

    std::uint64_t pos = 0;

    bool available(std::uint64_t n) const { return pos <= data_.size() && n <= data_.size() - pos; }
    std::string_view take(std::uint64_t n) {
        if (!available(n)) psd_error("the PSD file is cut short");
        const std::string_view out = data_.substr(static_cast<std::size_t>(pos), static_cast<std::size_t>(n));
        pos += n;
        return out;
    }
    // take(end - pos), where end may lie before pos (Python's negative n)
    std::string_view take_to(std::uint64_t end) {
        if (end < pos) psd_error("the PSD file is cut short");
        return take(end - pos);
    }
    unsigned u8() { return byte_at(take(1), 0); }
    unsigned u16() {
        const std::string_view b = take(2);
        return (byte_at(b, 0) << 8) | byte_at(b, 1);
    }
    int i16() { return static_cast<std::int16_t>(static_cast<std::uint16_t>(u16())); }
    std::uint32_t u32() { return be32(take(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() {
        const std::string_view b = take(8);
        return (std::uint64_t{be32(b.substr(0, 4))} << 32) | be32(b.substr(4, 4));
    }
    std::uint64_t length() { return psb_ ? u64() : u32(); }  // (PSB uses 8-byte lengths in some places)
    bool psb() const { return psb_; }
    std::string_view data() const { return data_; }

private:
    std::string_view data_;
    bool psb_;
};

// Python's bytes.decode("latin-1"): each byte the code point of its value, as UTF-8.
std::string latin1(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const unsigned b = static_cast<unsigned char>(c);
        if (b < 0x80) {
            out += c;
        } else {
            out += static_cast<char>(0xC0 | (b >> 6));
            out += static_cast<char>(0x80 | (b & 0x3F));
        }
    }
    return out;
}

void put_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Python's bytes.decode("utf-16-be", "replace") (CPython's decoder): a low surrogate alone, a high one not followed by
// a low one (that one is read again) and a high one at the end (with an odd byte after it) are each U+FFFD; so is an
// odd byte at the end.
std::string utf16be(std::string_view bytes) {
    std::string out;
    const std::size_t n = bytes.size();
    std::size_t i = 0;
    const auto unit = [&](std::size_t at) { return (byte_at(bytes, at) << 8) | byte_at(bytes, at + 1); };
    for (;;) {
        if (n - i < 2) {
            if (i != n) put_utf8(out, 0xFFFD);  // ("truncated data")
            return out;
        }
        const unsigned ch = unit(i);
        i += 2;
        if (ch < 0xD800 || ch > 0xDFFF) {
            put_utf8(out, ch);
            continue;
        }
        if (ch >= 0xDC00) {  // ("illegal encoding")
            put_utf8(out, 0xFFFD);
            continue;
        }
        if (n - i < 2) {  // ("unexpected end of data": the high surrogate and what is left)
            put_utf8(out, 0xFFFD);
            return out;
        }
        const unsigned low = unit(i);
        if (low < 0xDC00 || low > 0xDFFF) {  // ("illegal UTF-16 surrogate": the high one alone)
            put_utf8(out, 0xFFFD);
            continue;
        }
        i += 2;
        put_utf8(out, 0x10000 + ((ch - 0xD800) << 10) + (low - 0xDC00));
    }
}

const std::map<std::string, std::string, std::less<>>& blend_keys() {
    static const std::map<std::string, std::string, std::less<>> keys{
        {"norm", "normal"},     {"mul ", "multiply"},   {"scrn", "screen"},     {"lddg", "add"},
        {"over", "overlay"},    {"dark", "darken"},     {"lite", "lighten"},    {"idiv", "color_burn"},
        {"div ", "color_dodge"}, {"lbrn", "linear_burn"}, {"sLit", "soft_light"}, {"hLit", "hard_light"},
        {"diff", "difference"}, {"smud", "exclusion"},  {"fsub", "subtract"},   {"fdiv", "divide"},
        {"hue ", "hue"},        {"sat ", "saturation"}, {"colr", "color"},      {"lum ", "luminosity"},
        {"pass", "normal"}};
    return keys;
}

bool long_key(std::string_view tag) {
    static constexpr std::string_view kKeys[] = {"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn",
                                                 "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    return std::find(std::begin(kKeys), std::end(kKeys), tag) != std::end(kKeys);
}

bool adjust_tag(std::string_view tag) {
    static constexpr std::string_view kTags[] = {"SoCo", "GdFl", "PtFl", "levl", "curv", "brit", "hue2",
                                                 "blnc", "nvrt", "post", "thrs", "grdm", "selc", "mixr",
                                                 "phfl", "expA", "vibA", "blwh", "clrL"};
    return std::find(std::begin(kTags), std::end(kTags), tag) != std::end(kTags);
}

// The decoded bytes a file may make together (Python holds them all at once).
class Budget {
public:
    explicit Budget(std::int64_t limit) : left_(limit) {}
    void spend(std::uint64_t n) {
        if (n > static_cast<std::uint64_t>(left_)) too_large("the layers are too large to read");
        left_ -= static_cast<std::int64_t>(n);
    }

private:
    std::int64_t left_;
};

[[noreturn]] void zlib_error(int err, const z_stream& z) {
    // zlibmodule.c's zlib_error: zlib's own message, else one for the code
    const char* message = z.msg;
    if (message == nullptr) {
        if (err == Z_BUF_ERROR) message = "incomplete or truncated stream";
        if (err == Z_STREAM_ERROR) message = "inconsistent stream state";
        if (err == Z_DATA_ERROR) message = "invalid input data";
    }
    std::string text = "Error " + std::to_string(err) + " while decompressing data";
    if (message != nullptr) text += std::string(": ") + message;
    throw ReadError("error", text, false);  // (zlib.error: fileops.import_psd lets it through)
}

// zlib.decompress(data), as CPython 3.12 runs it (the whole input at once with Z_FINISH, in chunks of 4 GiB): how many
// bytes it makes, and its first `keep` bytes in `out` (when given); every byte it makes is spent from `budget`. zlib's
// errors as zlib.error.
std::uint64_t inflate_all(std::string_view input, std::uint64_t keep, std::string* out, Budget& budget) {
    z_stream z{};
    if (inflateInit2(&z, MAX_WBITS) != Z_OK) throw core::Error("memory", "zlib cannot start");
    struct Ender {
        z_stream* z;
        ~Ender() { inflateEnd(z); }
    } ender{&z};
    std::string scratch(std::size_t{1} << 16, '\0');
    std::size_t given = 0;
    std::uint64_t made = 0;
    int err = Z_OK;
    do {
        const std::size_t chunk = std::min<std::size_t>(input.size() - given, UINT_MAX);
        z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data() + given));
        z.avail_in = static_cast<uInt>(chunk);
        given += chunk;
        const int flush = given == input.size() ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = reinterpret_cast<Bytef*>(scratch.data());
            z.avail_out = static_cast<uInt>(scratch.size());
            err = inflate(&z, flush);
            if (err != Z_OK && err != Z_BUF_ERROR && err != Z_STREAM_END) {
                if (err == Z_MEM_ERROR) throw core::Error("memory", "out of memory while decompressing data");
                zlib_error(err, z);
            }
            const std::size_t now = scratch.size() - z.avail_out;
            budget.spend(now);
            if (out != nullptr && made < keep) out->append(scratch.data(), static_cast<std::size_t>(std::min<std::uint64_t>(now, keep - made)));
            made += now;
        } while (z.avail_out == 0);
    } while (err != Z_STREAM_END && given < input.size());
    if (err != Z_STREAM_END) zlib_error(err, z);
    return made;
}

// psd._unpackbits(data, size) into `out` (exactly size bytes, zeros after what the data makes).
void unpackbits(std::string_view data, char* out, std::uint64_t size) {
    std::uint64_t o = 0;
    std::size_t i = 0;
    const std::size_t n = data.size();
    while (i < n && o < size) {
        const unsigned c = byte_at(data, i++);
        if (c < 128) {
            const std::size_t lit = std::min<std::size_t>(c + 1, n - i);
            const std::uint64_t put = std::min<std::uint64_t>(lit, size - o);
            std::memcpy(out + o, data.data() + i, static_cast<std::size_t>(put));
            o += put;
            i += c + 1;
        } else if (c > 128) {
            if (i < n) {
                const std::uint64_t put = std::min<std::uint64_t>(257 - c, size - o);
                std::memset(out + o, data[i], static_cast<std::size_t>(put));
                o += put;
            }
            i += 1;
        }
    }
    if (o < size) std::memset(out + o, 0, static_cast<std::size_t>(size - o));
}

// What one channel decodes to: its 8-bit samples when kept, else only how many there are.
struct Samples {
    std::string bytes;
    std::uint64_t length = 0;
};

// The row bytes of a channel of `width` samples (width > 0) at `depth`.
std::uint64_t row_bytes(std::int64_t width, int depth) {
    const auto w = static_cast<std::uint64_t>(width);
    return depth == 1 ? (w + 7) / 8 : w * static_cast<std::uint64_t>(std::max(1, depth / 8));
}

// psd._depth8(raw, width, height, depth): samples of any depth as 8-bit, from `length` raw bytes (at most the
// channel's size); `raw` holds them when they are wanted (else only what Python would raise, and the count).
Samples depth8(const std::string* raw, std::uint64_t length, std::int64_t width, std::int64_t height, int depth) {
    Samples out;
    if (depth == 8) {
        out.length = length;
        if (raw != nullptr) out.bytes = *raw;
        return out;
    }
    if (depth == 16) {  // (raw[0::2]: the high byte of each)
        out.length = (length + 1) / 2;
        if (raw != nullptr) {
            out.bytes.resize(static_cast<std::size_t>(out.length));
            for (std::size_t i = 0; i < out.bytes.size(); ++i) out.bytes[i] = (*raw)[2 * i];
        }
        return out;
    }
    if (depth == 32) {
        if (length % 4 != 0) value_error("buffer size must be a multiple of element size");  // (np.frombuffer)
        out.length = length / 4;
        if (raw != nullptr) {
            // (np.clip(f, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8): float32 throughout, the power numpy's (libm's
            // powf, as the reference runs numpy), NaN cast to 0 as numpy casts it there
            const float gamma = static_cast<float>(1 / 2.2);
            out.bytes.resize(static_cast<std::size_t>(out.length));
            for (std::size_t i = 0; i < out.bytes.size(); ++i) {
                const std::uint32_t bits = be32(std::string_view(*raw).substr(4 * i, 4));
                float f = 0.0f;
                std::memcpy(&f, &bits, 4);
                unsigned char v = 0;
                if (!std::isnan(f)) {
                    const float clipped = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
                    const float scaled = np::powf(clipped, gamma) * 255.0f;
                    v = static_cast<unsigned char>(static_cast<int>(scaled));
                }
                out.bytes[i] = static_cast<char>(v);
            }
        }
        return out;
    }
    if (depth == 1) {
        // Image.frombytes("1", (width, height), raw), as L inverted (a set bit is black)
        if (width <= 0 || height <= 0) return out;
        const std::uint64_t rowbytes = row_bytes(width, 1);
        if (length < rowbytes * static_cast<std::uint64_t>(height)) value_error("not enough image data");
        out.length = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
        if (raw != nullptr) {
            out.bytes.resize(static_cast<std::size_t>(out.length));
            std::size_t k = 0;
            for (std::int64_t y = 0; y < height; ++y) {
                const std::size_t row = static_cast<std::size_t>(rowbytes) * static_cast<std::size_t>(y);
                for (std::int64_t x = 0; x < width; ++x) {
                    const bool set = ((byte_at(*raw, row + static_cast<std::size_t>(x / 8)) >> (7 - x % 8)) & 1) != 0;
                    out.bytes[k++] = static_cast<char>(set ? 0 : 255);
                }
            }
        }
        return out;
    }
    psd_error("PSD depth " + std::to_string(depth) + " is not supported");
}

// psd._channel(r, width, height, depth, end=end): one channel's samples from the reader's position, as 8-bit; the
// reader is left at `end`. With `keep` false, only checked (what Python would raise) and counted.
Samples read_channel(Reader& r, std::int64_t width, std::int64_t height, int depth, std::uint64_t end, bool keep,
                     Budget& budget, const Limits& limits) {
    const unsigned compression = r.u16();
    if (width <= 0 || height <= 0) {
        r.pos = end;
        return {};
    }
    const std::uint64_t rowbytes = row_bytes(width, depth);
    const std::uint64_t size = held_product(rowbytes, static_cast<std::uint64_t>(height));
    const std::uint64_t most = static_cast<std::uint64_t>(limits.max_channel_bytes);
    std::string raw;
    std::uint64_t length = 0;  // (of raw[:size])
    if (compression == 0) {
        const std::string_view bytes = r.take(size);
        if (size > most) too_large("a layer is too large to read");
        budget.spend(size);
        length = size;
        if (keep) raw.assign(bytes);
    } else if (compression == 1) {
        // the rows' byte counts (all read first), then each row PackBits-unpacked to its width
        const unsigned count_bytes = r.psb() ? 4 : 2;
        if (!r.available(held_product(static_cast<std::uint64_t>(height), count_bytes))) psd_error("the PSD file is cut short");
        const std::uint64_t counts_at = r.pos;
        r.pos += static_cast<std::uint64_t>(height) * count_bytes;
        const std::string_view data = r.data();
        const auto count = [&](std::uint64_t row) {
            const std::string_view b = data.substr(static_cast<std::size_t>(counts_at + row * count_bytes), count_bytes);
            return count_bytes == 4 ? std::uint64_t{be32(b)} : std::uint64_t{(byte_at(b, 0) << 8) | byte_at(b, 1)};
        };
        // (Python pads each row to its width as it goes: a channel too large is refused before its rows are read)
        if (size > most) too_large("a layer is too large to read");
        budget.spend(size);
        std::uint64_t total = 0;
        for (std::uint64_t y = 0; y < static_cast<std::uint64_t>(height); ++y) total += count(y);
        if (!r.available(total)) psd_error("the PSD file is cut short");
        length = size;
        if (keep) {
            raw.resize(static_cast<std::size_t>(size));
            for (std::uint64_t y = 0; y < static_cast<std::uint64_t>(height); ++y) {
                unpackbits(r.take(count(y)), raw.data() + y * rowbytes, rowbytes);
            }
        }
    } else if (compression == 2 || compression == 3) {
        const std::string_view packed = r.take_to(end);
        // (what is kept is raw[:size]: at most the channel's size, and no more than a channel may be)
        const std::uint64_t made = inflate_all(packed, std::min(size, most + 1), keep ? &raw : nullptr, budget);
        length = std::min(made, size);
        if (length > most) too_large("a layer is too large to read");
        if (compression == 3) {  // (each row stored as differences from the sample before)
            const int bpp = std::max(1, depth / 8);
            if (bpp != 1 && bpp != 2 && bpp != 4) throw ReadError("KeyError", std::to_string(bpp), false);
            if (length % static_cast<std::uint64_t>(bpp) != 0) value_error("buffer size must be a multiple of element size");
            const std::uint64_t elements = length / static_cast<std::uint64_t>(bpp);
            if (elements != static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height)) {
                value_error("cannot reshape array of size " + std::to_string(elements) + " into shape (" + std::to_string(height) +
                            "," + std::to_string(width) + ")");
            }
            if (keep) {
                // np.cumsum along each row, modulo the sample's size
                const auto step = static_cast<std::size_t>(bpp);
                const std::uint32_t mask = bpp == 4 ? 0xFFFFFFFFu : (std::uint32_t{1} << (8 * bpp)) - 1;
                for (std::int64_t y = 0; y < height; ++y) {
                    std::uint32_t sum = 0;
                    std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * step;
                    for (std::int64_t x = 0; x < width; ++x, at += step) {
                        std::uint32_t v = 0;
                        for (std::size_t k = 0; k < step; ++k) v = (v << 8) | byte_at(raw, at + k);
                        sum = (sum + v) & mask;
                        for (std::size_t k = 0; k < step; ++k) raw[at + k] = static_cast<char>((sum >> (8 * (step - 1 - k))) & 0xFF);
                    }
                }
            }
        }
    } else {
        psd_error("unknown PSD compression " + std::to_string(compression));
    }
    r.pos = end;
    return depth8(keep ? &raw : nullptr, length, width, height, depth);
}

int colour_bands(int mode) { return mode == 4 ? 4 : mode == 3 ? 3 : 1; }

// The bands psd._to_rgba reads for a mode: CMYK's four, a grey band, or red, green and blue.
std::vector<int> bands_of(int mode) {
    if (mode == 4) return {0, 1, 2, 3};
    if (mode == 1 || mode == 0 || mode == 2 || mode == 8) return {0};
    return {0, 1, 2};
}

// psd._to_rgba(mode, channels, size)
Image to_rgba(int mode, const std::map<int, std::string>& channels, Size size) {
    const auto band = [&](int id, int fallback) {
        const auto found = channels.find(id);
        if (found != channels.end() && !found->second.empty()) return Image::frombytes("L", size, found->second);
        return Image::create("L", size, Ink(fallback));
    };
    Image rgb;
    if (mode == 4) {  // CMYK (stored inverted: 255 is no ink)
        std::vector<Image> inks;
        for (int i = 0; i < 4; ++i) inks.push_back(ops::invert(band(i, 255)));
        rgb = Image::merge("CMYK", inks).convert("RGB");
    } else if (mode == 1 || mode == 0 || mode == 2 || mode == 8) {  // grayscale, bitmap, indexed (as grey), duotone
        const Image g = band(0, 255);
        rgb = Image::merge("RGB", {g, g, g});
    } else {
        rgb = Image::merge("RGB", {band(0, 0), band(1, 0), band(2, 0)});
    }
    Image rgba = rgb.convert("RGBA");
    rgba.putalpha(band(-1, 255));
    return rgba;
}

int as_int(std::int64_t v) { return static_cast<int>(v); }

// Image.new and Image.frombytes of (width, height), as Pillow refuses them before a pixel is made (neither read_psd nor
// fileops catches these): a side past C's int (OverflowError), a width past Storage.c's line size check (MemoryError,
// without words).
void pillow_size(std::int64_t width, std::int64_t height) {
    if (width > INT_MAX || height > INT_MAX) throw ReadError("OverflowError", "signed integer is greater than maximum", false);
    if (width > INT_MAX / 4 - 1) throw ReadError("MemoryError", "", false);
}

}  // namespace

std::string blend_of(std::string_view key) {
    const auto found = blend_keys().find(key);
    return found == blend_keys().end() ? std::string("normal") : found->second;
}

File File::read(std::string data, const Limits& limits) {
    File f;
    f.data_ = std::move(data);
    f.limits_ = limits;
    const std::string_view d = f.data_;
    if (d.substr(0, 4) != "8BPS") psd_error("not a PSD file");
    if (d.size() < 6) throw ReadError("error", "unpack requires a buffer of 2 bytes", true);  // (struct.error)
    const unsigned version = (byte_at(d, 4) << 8) | byte_at(d, 5);
    if (version != 1 && version != 2) psd_error("unknown PSD version");
    f.psb_ = version == 2;
    Reader r(d, f.psb_);
    r.take(12);
    f.nchan_ = static_cast<int>(r.u16());
    f.height_ = r.u32();
    f.width_ = r.u32();
    f.depth_ = static_cast<int>(r.u16());
    f.mode_ = static_cast<int>(r.u16());
    if (f.mode_ == 9) psd_error("Lab PSD files are not supported: save it as RGB or CMYK");
    r.take(r.u32());  // colour mode data
    std::uint64_t res_end = r.u32();
    res_end += r.pos;
    while (r.pos + 12 <= res_end) {
        if (r.take(4) != "8BIM") break;
        const unsigned rid = r.u16();
        const unsigned nlen = r.u8();
        r.take(nlen + (1 - nlen % 2));  // (the Pascal name, padded to even with its length byte)
        const std::uint32_t size = r.u32();
        const std::string_view block = r.take(std::uint64_t{size} + size % 2);
        if (rid == 1005 && size >= 4) {
            const std::uint32_t fixed = be32(block);
            f.dpi_ = fixed == 0 ? 72.0 : fixed / 65536.0;
        }
    }
    r.pos = res_end;
    static const std::map<int, std::string> kModeNames{{0, "bitmap"}, {1, "gray"},         {2, "indexed"}, {3, "rgb"},
                                                       {4, "cmyk"},   {7, "multichannel"}, {8, "duotone"}, {9, "lab"}};
    const auto mode_name = kModeNames.find(f.mode_);
    f.mode_name_ = mode_name != kModeNames.end() ? mode_name->second : std::to_string(f.mode_);

    struct Entry {
        int id;
        std::uint64_t length;
    };
    std::vector<std::vector<Entry>> entries;
    const std::uint64_t lm_len = r.length();
    const std::uint64_t lm_end = past(r.pos, lm_len);
    if (lm_len != 0) {
        const std::uint64_t li_len = r.length();
        const std::uint64_t li_end = past(r.pos, li_len);
        if (li_len != 0) {
            const int count = std::abs(r.i16());
            for (int n = 0; n < count; ++n) {
                Record rec;
                rec.top = r.i32();
                rec.left = r.i32();
                rec.bottom = r.i32();
                rec.right = r.i32();
                std::vector<Entry> chans;
                const unsigned nchans = r.u16();
                for (unsigned k = 0; k < nchans; ++k) {
                    const int id = r.i16();
                    chans.push_back(Entry{id, r.length()});
                }
                const std::string_view sig = r.take(4);
                if (sig != "8BIM" && sig != "8B64") psd_error("broken PSD layer record");
                rec.blend = blend_of(r.take(4));
                rec.opacity = static_cast<int>(r.u8());
                rec.clip = r.u8() == 1;
                rec.visible = (r.u8() & 2) == 0;
                r.u8();
                std::uint64_t extra_end = r.u32();
                extra_end += r.pos;
                const std::uint32_t mlen = r.u32();
                if (mlen != 0) {
                    const std::uint64_t mend = r.pos + mlen;
                    rec.mask_top = r.i32();
                    rec.mask_left = r.i32();
                    rec.mask_bottom = r.i32();
                    rec.mask_right = r.i32();
                    rec.mask_default = static_cast<int>(r.u8());
                    rec.mask_disabled = (r.u8() & 2) != 0;
                    rec.mask = true;
                    r.pos = mend;
                }
                const std::uint32_t ranges = r.u32();  // blending ranges
                r.pos += ranges;
                const unsigned nlen = r.u8();
                rec.name = latin1(r.take(nlen));
                r.pos += (4 - (nlen + 1) % 4) % 4;
                while (r.pos + 12 <= extra_end) {
                    const std::string_view block_sig = r.take(4);
                    if (block_sig != "8BIM" && block_sig != "8B64") break;
                    const std::string_view tag = r.take(4);
                    const std::uint64_t size = r.psb() && long_key(tag) ? r.u64() : r.u32();
                    const std::string_view body = r.take(size);
                    if (tag == "luni" && body.size() >= 4) {
                        const std::uint64_t units = be32(body);
                        std::string name = utf16be(body.substr(4, static_cast<std::size_t>(std::min<std::uint64_t>(2 * units, body.size() - 4))));
                        while (!name.empty() && name.back() == '\0') name.pop_back();
                        rec.name = std::move(name);
                    } else if ((tag == "lsct" || tag == "lsdk") && body.size() >= 4) {
                        rec.section = be32(body);
                    } else if (tag == "TySh") {
                        rec.kind = "text";
                    } else if (adjust_tag(tag)) {
                        rec.kind = "adjust";
                    }
                    if (size % 2 != 0 && r.pos < extra_end) {
                        const std::string_view next = r.pos < d.size() ? d.substr(static_cast<std::size_t>(r.pos), 4) : std::string_view();
                        if (next != "8BIM" && next != "8B64") r.pos += 1;
                    }
                }
                r.pos = extra_end;
                f.records_.push_back(std::move(rec));
                entries.push_back(std::move(chans));
            }
            // every channel decoded once, in the file's order, for what read_psd would raise (nothing kept)
            Budget budget(limits.max_decoded_bytes);
            for (std::size_t n = 0; n < f.records_.size(); ++n) {
                Record& rec = f.records_[n];
                const std::int64_t w = rec.right - rec.left;
                const std::int64_t h = rec.bottom - rec.top;
                for (const Entry& entry : entries[n]) {
                    const std::uint64_t end = past(r.pos, entry.length);
                    if (entry.length < 2) {
                        r.pos = end;
                        continue;
                    }
                    ChannelRef ref;
                    ref.id = entry.id;
                    ref.start = r.pos;
                    ref.end = end;
                    if (entry.id == -2 && rec.mask) {
                        ref.width = rec.mask_right - rec.mask_left;
                        ref.height = rec.mask_bottom - rec.mask_top;
                    } else if (entry.id < -2) {
                        r.pos = end;
                        continue;
                    } else {
                        ref.width = w;
                        ref.height = h;
                    }
                    ref.length = read_channel(r, ref.width, ref.height, f.depth_, end, false, budget, limits).length;
                    const auto same = std::find_if(rec.data.begin(), rec.data.end(), [&](const ChannelRef& c) { return c.id == ref.id; });
                    if (same != rec.data.end()) {
                        *same = ref;
                    } else {
                        rec.data.push_back(ref);
                    }
                }
            }
        }
        r.pos = li_end;
    }
    r.pos = lm_end;
    f.merged_at_ = r.pos;
    // (read_psd makes the merged picture here, before the layers' pictures)
    if (f.plan_merged().reaches) pillow_size(f.width_, f.height_);

    // the layers, bottom to top, folders after the layers in them (read_psd's loop), with the errors its pictures
    // would raise as they are made
    std::vector<std::vector<std::size_t>> pending;
    for (std::size_t n = 0; n < f.records_.size(); ++n) {
        const Record& rec = f.records_[n];
        if (rec.section == 3) {  // (the end of a folder, seen first from the bottom)
            pending.emplace_back();
            continue;
        }
        if (rec.section == 1 || rec.section == 2) {
            Layer folder;
            folder.name = rec.name.empty() ? std::string("フォルダー") : rec.name;
            folder.opacity = rec.opacity / 255.0;
            folder.visible = rec.visible;
            folder.blend = rec.blend;
            folder.folder = true;
            folder.kind = "folder";
            std::vector<std::size_t> kids;
            if (!pending.empty()) {
                kids = std::move(pending.back());
                pending.pop_back();
            }
            f.layers_.push_back(std::move(folder));
            const std::size_t at = f.layers_.size() - 1;
            for (const std::size_t k : kids) f.layers_[k].parent = at;
            if (!pending.empty()) pending.back().push_back(at);
            continue;
        }
        const std::int64_t w = rec.right - rec.left;
        const std::int64_t h = rec.bottom - rec.top;
        const bool colour = std::any_of(rec.data.begin(), rec.data.end(), [](const ChannelRef& c) { return c.id >= 0; });
        if (rec.kind == "adjust" || w <= 0 || h <= 0 || !colour) {
            f.skipped_.push_back(rec.name);
            continue;
        }
        // _to_rgba: Image.frombytes("L", size, data) of a band with too few samples
        if (static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > static_cast<std::uint64_t>(limits.max_layer_pixels)) {
            too_large("a layer is too large to read");
        }
        std::vector<int> used = bands_of(f.mode_);
        used.push_back(-1);
        for (const int id : used) {
            const ChannelRef* c = rec.channel(id);
            if (c != nullptr && c->length > 0 && c->length < static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h)) {
                value_error("not enough image data");
            }
        }
        pillow_size(f.width_, f.height_);  // (the layer's picture over the whole canvas)
        Layer layer;
        layer.name = rec.name.empty() ? std::string("レイヤー") : rec.name;
        layer.opacity = rec.opacity / 255.0;
        layer.visible = rec.visible;
        layer.blend = rec.blend;
        layer.clip = rec.clip;
        layer.kind = rec.kind;
        layer.record = n;
        const ChannelRef* mask = rec.channel(-2);
        if (rec.mask && mask != nullptr && !rec.mask_disabled) {
            layer.has_mask = true;
            if (mask->width > 0 && mask->height > 0 && mask->length > 0) {
                const std::uint64_t pixels = static_cast<std::uint64_t>(mask->width) * static_cast<std::uint64_t>(mask->height);
                if (pixels > static_cast<std::uint64_t>(limits.max_layer_pixels)) too_large("a layer is too large to read");
                if (mask->length < pixels) value_error("not enough image data");
            }
        }
        f.layers_.push_back(std::move(layer));
        if (!pending.empty()) pending.back().push_back(f.layers_.size() - 1);
    }
    return f;
}

std::string File::samples(const ChannelRef& channel) const {
    Reader r(data_, psb_);
    r.pos = channel.start;
    Budget budget(limits_.max_decoded_bytes);
    return read_channel(r, channel.width, channel.height, depth_, channel.end, true, budget, limits_).bytes;
}

Image File::part(const Record& record) const {
    std::map<int, std::string> channels;
    std::vector<int> used = bands_of(mode_);
    used.push_back(-1);
    for (const int id : used) {
        if (const ChannelRef* c = record.channel(id)) channels[id] = samples(*c);
    }
    return to_rgba(mode_, channels, Size{as_int(record.right - record.left), as_int(record.bottom - record.top)});
}

namespace {

// (width and height are 32-bit: their product fits 64 bits unsigned)
void canvas_fits(std::int64_t width, std::int64_t height) {
    if (static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > static_cast<std::uint64_t>(limits::kPixels)) {
        too_large("the picture is too large to read");
    }
}

}  // namespace

Image File::image(const Layer& layer) const {
    canvas_fits(width_, height_);
    const Record& record = records_.at(layer.record);
    Image canvas = Image::create("RGBA", Size{as_int(width_), as_int(height_)}, Ink{0, 0, 0, 0});
    canvas.paste(part(record), Point{as_int(record.left), as_int(record.top)});
    return canvas;
}

std::optional<Image> File::mask(const Layer& layer) const {
    if (!layer.has_mask) return std::nullopt;
    canvas_fits(width_, height_);
    const Record& record = records_.at(layer.record);
    Image mask = Image::create("L", Size{as_int(width_), as_int(height_)}, Ink(record.mask_default));
    const ChannelRef* channel = record.channel(-2);
    if (channel != nullptr && channel->width > 0 && channel->height > 0 && channel->length > 0) {
        const std::string data = samples(*channel);
        mask.paste(Image::frombytes("L", Size{as_int(channel->width), as_int(channel->height)}, data),
                   Point{as_int(record.mask_left), as_int(record.mask_top)});
    }
    return mask;
}

File::MergedPlan File::plan_merged() const {
    // read_psd's merged picture: None for anything it cannot read (it catches PSDError, ValueError and struct.error)
    MergedPlan plan;
    Reader r(data_, psb_);
    r.pos = merged_at_;
    if (!r.available(2)) return plan;
    plan.compression = r.u16();
    const std::uint64_t rowbytes = depth_ == 1 ? (static_cast<std::uint64_t>(width_) + 7) / 8
                                               : static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(std::max(1, depth_ / 8));
    const auto rows = static_cast<std::uint64_t>(height_);
    const auto planes = static_cast<std::uint64_t>(nchan_);
    const unsigned count_bytes = psb_ ? 4 : 2;
    const std::string_view d = data_;
    // every plane is read in turn (each row's bytes taken: one it cannot take makes it None, as does a depth _depth8
    // refuses); a 1-bit plane is made a picture as soon as it is read
    plan.starts.assign(static_cast<std::size_t>(std::min<std::uint64_t>(planes, 5)), r.pos);
    bool first = false;  // (the first plane's bytes are there)
    bool all = false;
    if (plan.compression == 1) {
        if (!r.available(held_product(rows * planes, count_bytes))) return plan;
        plan.counts_at = r.pos;
        r.pos += rows * planes * count_bytes;
        std::uint64_t at = r.pos;
        for (std::uint64_t c = 0; c < planes; ++c) {
            if (c < plan.starts.size()) plan.starts[static_cast<std::size_t>(c)] = at;
            for (std::uint64_t y = 0; y < rows; ++y) {
                const std::string_view b = d.substr(static_cast<std::size_t>(plan.counts_at + (c * rows + y) * count_bytes), count_bytes);
                at += count_bytes == 4 ? std::uint64_t{be32(b)} : std::uint64_t{(byte_at(b, 0) << 8) | byte_at(b, 1)};
            }
            if (c == 0) first = r.available(at - r.pos);
        }
        all = r.available(at - r.pos);
    } else if (plan.compression == 0) {
        const std::uint64_t plane = held_product(rowbytes, rows);
        first = r.available(plane);
        all = r.available(held_product(plane, planes));
        for (std::size_t c = 0; c < plan.starts.size(); ++c) plan.starts[c] = r.pos + plane * c;
    } else {
        return plan;
    }
    if (planes == 0) return plan;
    const int colour = colour_bands(mode_);
    const bool supported = depth_ == 1 || depth_ == 8 || depth_ == 16 || depth_ == 32;
    plan.reaches = depth_ == 1 ? first : supported && all && planes >= static_cast<std::uint64_t>(colour);
    plan.exists = supported && all && planes >= static_cast<std::uint64_t>(colour);
    return plan;
}

std::optional<Image> File::merged() const {
    const MergedPlan plan = plan_merged();
    if (!plan.exists) return std::nullopt;
    canvas_fits(width_, height_);
    const std::uint64_t rowbytes = depth_ == 1 ? (static_cast<std::uint64_t>(width_) + 7) / 8
                                               : static_cast<std::uint64_t>(width_) * static_cast<std::uint64_t>(std::max(1, depth_ / 8));
    const auto rows = static_cast<std::uint64_t>(height_);
    const unsigned count_bytes = psb_ ? 4 : 2;
    const std::string_view d = data_;
    const auto plane_samples = [&](std::size_t c) {
        std::string raw(static_cast<std::size_t>(rowbytes * rows), '\0');
        if (plan.compression == 0) {
            if (!raw.empty()) std::memcpy(raw.data(), d.data() + plan.starts[c], raw.size());
        } else {
            std::uint64_t at = plan.starts[c];
            for (std::uint64_t y = 0; y < rows; ++y) {
                const std::string_view b = d.substr(static_cast<std::size_t>(plan.counts_at + (c * rows + y) * count_bytes), count_bytes);
                const std::uint64_t n = count_bytes == 4 ? std::uint64_t{be32(b)} : std::uint64_t{(byte_at(b, 0) << 8) | byte_at(b, 1)};
                unpackbits(d.substr(static_cast<std::size_t>(at), static_cast<std::size_t>(n)), raw.data() + rowbytes * y, rowbytes);
                at += n;
            }
        }
        return depth8(&raw, raw.size(), width_, height_, depth_).bytes;
    };
    std::map<int, std::string> channels;
    const int colour = colour_bands(mode_);
    for (int c = 0; c < colour; ++c) channels[c] = plane_samples(static_cast<std::size_t>(c));
    if (static_cast<std::uint64_t>(nchan_) > static_cast<std::uint64_t>(colour) && records_.empty()) {
        channels[-1] = plane_samples(static_cast<std::size_t>(colour));  // (a flat file's transparency)
    }
    return to_rgba(mode_, channels, Size{as_int(width_), as_int(height_)});
}

}  // namespace genko::render::psd
