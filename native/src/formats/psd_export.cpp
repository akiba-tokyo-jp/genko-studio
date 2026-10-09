// PSD export (Python's genko/psd.py, its writer): one file a page with real layers (render::page_layers), each stored
// cut to what it holds and PackBits-compressed, the printed page as the merged picture. Layer names in Unicode (the
// luni block) with an ASCII one beside it.

#include <algorithm>
#include <map>

#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "formats/export.hpp"
#include "formats/output.hpp"
#include "render/page.hpp"

namespace genko::formats {

using render::Image;

namespace {

void put_u8(std::string& out, unsigned v) { out += static_cast<char>(v & 0xff); }
void put_u16(std::string& out, std::uint32_t v) {
    put_u8(out, v >> 8);
    put_u8(out, v);
}
void put_u32(std::string& out, std::uint32_t v) {
    put_u16(out, v >> 16);
    put_u16(out, v & 0xffff);
}
void put_i32(std::string& out, std::int32_t v) { put_u32(out, static_cast<std::uint32_t>(v)); }

// struct.pack(">I", n): struct.error past 32 bits
std::uint32_t u32_of(std::uint64_t n) {
    if (n > 0xffffffffULL) throw core::Error("value", "'I' format requires 0 <= number <= 4294967295");
    return static_cast<std::uint32_t>(n);
}

// psd._packbits_rows of one channel ("L"): each row's byte count (as numpy's ">u2": modulo 65536) and the packed
// bytes. Runs of 3 or more equal bytes are repeat packets, the rest literal packets, 128 bytes at most each; a run never
// crosses a row.
void packbits_rows(const Image& channel, std::string& counts, std::string& packed) {
    const int width = channel.width();
    const std::string pixels = channel.tobytes();
    for (int y = 0; y < channel.height(); ++y) {
        const unsigned char* row = reinterpret_cast<const unsigned char*>(pixels.data()) + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        std::uint64_t size = 0;
        // the runs of the row, then segments: a long run alone, consecutive short runs together
        int at = 0;
        while (at < width) {
            int run_end = at + 1;
            while (run_end < width && row[run_end] == row[at]) ++run_end;
            const bool long_run = run_end - at >= 3;
            int seg_end = run_end;
            if (!long_run) {  // (short runs up to the next long one)
                while (seg_end < width) {
                    int next = seg_end + 1;
                    while (next < width && row[next] == row[seg_end]) ++next;
                    if (next - seg_end >= 3) break;
                    seg_end = next;
                }
            }
            for (int start = at; start < seg_end; start += 128) {
                const int n = std::min(128, seg_end - start);
                if (long_run && n >= 2) {
                    packed += static_cast<char>((257 - n) & 0xff);
                    packed += static_cast<char>(row[start]);
                    size += 2;
                } else {
                    packed += static_cast<char>(n - 1);
                    packed.append(reinterpret_cast<const char*>(row + start), static_cast<std::size_t>(n));
                    size += static_cast<std::uint64_t>(n) + 1;
                }
            }
            at = seg_end;
        }
        put_u16(counts, static_cast<std::uint32_t>(size % 65536));
    }
}

// psd._rle: compression 1, every row's count, the packed rows.
std::string rle(const Image& channel) {
    std::string counts, packed;
    packbits_rows(channel.mode() == "L" ? channel : channel.convert("L"), counts, packed);
    std::string out;
    put_u16(out, 1);
    return out + counts + packed;
}

// name.encode("ascii", "replace")[:255] with its length, padded to 4 bytes
std::string pascal(const std::string& name) {
    std::string raw;
    for (std::size_t at = 0; at < name.size();) {
        const auto lead = static_cast<unsigned char>(name[at]);
        const std::size_t n = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        raw += n == 1 ? name[at] : '?';
        at += n;
    }
    if (raw.size() > 255) raw.resize(255);
    std::string payload(1, static_cast<char>(raw.size()));
    payload += raw;
    payload.append((4 - payload.size() % 4) % 4, '\0');
    return payload;
}

// The luni block: the name in UTF-16 (big-endian) with its length in code units, padded to 4 bytes.
std::string unicode_name(const std::string& name) {
    std::string text;
    for (std::size_t at = 0; at < name.size();) {
        const auto lead = static_cast<unsigned char>(name[at]);
        std::uint32_t cp = lead;
        std::size_t n = 1;
        if (lead >= 0xf0) {
            cp = lead & 0x07u;
            n = 4;
        } else if (lead >= 0xe0) {
            cp = lead & 0x0fu;
            n = 3;
        } else if (lead >= 0xc0) {
            cp = lead & 0x1fu;
            n = 2;
        }
        for (std::size_t k = 1; k < n && at + k < name.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(name[at + k]) & 0x3fu);
        at += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put_u16(text, 0xd800 + (cp >> 10));
            put_u16(text, 0xdc00 + (cp & 0x3ff));
        } else {
            put_u16(text, cp);
        }
    }
    std::string data;
    put_u32(data, static_cast<std::uint32_t>(text.size() / 2));
    data += text;
    data.append((4 - data.size() % 4) % 4, '\0');
    std::string out = "8BIMluni";
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    return out + data;
}

// psd.BLEND_BACK: the key of each blend mode (the first of BLEND_KEYS for it), b"norm" for one it does not know.
std::string blend_key(const std::string& blend) {
    static const std::map<std::string, std::string> keys{
        {"normal", "norm"},     {"multiply", "mul "},    {"screen", "scrn"},     {"add", "lddg"},         {"overlay", "over"},
        {"darken", "dark"},     {"lighten", "lite"},     {"color_burn", "idiv"}, {"color_dodge", "div "}, {"linear_burn", "lbrn"},
        {"soft_light", "sLit"}, {"hard_light", "hLit"},  {"difference", "diff"}, {"exclusion", "smud"},   {"subtract", "fsub"},
        {"divide", "fdiv"},     {"hue", "hue "},         {"saturation", "sat "}, {"color", "colr"},       {"luminosity", "lum "}};
    const auto it = keys.find(blend.empty() ? std::string("normal") : blend);
    return it == keys.end() ? "norm" : it->second;
}

// psd._layer_records, one layer at a time: its record and its channels' data.
class Records {
public:
    explicit Records(render::Size size) : size_(size) {}

    void add(render::PageLayer&& layer) {
        const Image rgba = layer.image.mode() == "RGBA" ? std::move(layer.image) : layer.image.convert("RGBA");
        const std::optional<render::Box> found = rgba.getbbox();
        const render::Box box = found ? *found : render::Box{0, 0, 1, 1};  // (an empty layer kept as 1 pixel)
        const Image crop = rgba.crop(box);
        const std::vector<Image> bands = crop.split();
        put_i32(records_, box.y0);
        put_i32(records_, box.x0);
        put_i32(records_, box.y1);
        put_i32(records_, box.x1);
        std::vector<std::pair<int, std::string>> channels{{-1, rle(bands[3])}, {0, rle(bands[0])}, {1, rle(bands[1])}, {2, rle(bands[2])}};
        std::string mask_data;
        if (layer.mask) {  // (an L picture over the whole canvas: white shows)
            channels.emplace_back(-2, rle(layer.mask->convert("L").resize(size_)));
            put_u32(mask_data, 20);
            put_i32(mask_data, 0);
            put_i32(mask_data, 0);
            put_i32(mask_data, size_.height);
            put_i32(mask_data, size_.width);
            mask_data += std::string("\xff\x00\x00\x00", 4);
        } else {
            put_u32(mask_data, 0);
        }
        put_u16(records_, static_cast<std::uint32_t>(channels.size()));
        for (const auto& [id, packed] : channels) {
            put_u16(records_, static_cast<std::uint32_t>(static_cast<std::int16_t>(id)) & 0xffff);
            put_u32(records_, u32_of(packed.size()));
        }
        const double opacity_given = layer.settings ? layer.opacity : 1.0;
        const auto opacity = static_cast<unsigned>(std::clamp<std::int64_t>(core::py_round_int(255 * opacity_given), 0, 255));
        records_ += "8BIM" + blend_key(layer.settings ? layer.blend : std::string("normal"));
        put_u8(records_, opacity);
        put_u8(records_, layer.settings && layer.clip ? 1 : 0);
        put_u8(records_, 0);  // (flags: visible)
        put_u8(records_, 0);
        std::string extra = mask_data;
        put_u32(extra, 0);
        extra += pascal(layer.name) + unicode_name(layer.name);
        put_u32(records_, u32_of(extra.size()));
        records_ += extra;
        for (const auto& [id, packed] : channels) data_ += packed;
        ++count_;
    }

    std::string finish() {
        std::string info;
        put_u16(info, static_cast<std::uint32_t>(static_cast<std::int16_t>(count_)) & 0xffff);
        info += records_;
        info += data_;
        if (info.size() % 2) info += '\0';
        std::string payload;
        put_u32(payload, u32_of(info.size()));
        payload += info;
        put_u32(payload, 0);  // no global layer mask
        std::string out;
        put_u32(out, u32_of(payload.size()));
        return out + payload;
    }

private:
    render::Size size_;
    std::string records_;
    std::string data_;
    int count_ = 0;
};

// psd._resources: ResolutionInfo (1005), the resolution in pixels per inch as 16.16 fixed point.
std::string resources(std::int64_t dpi) {
    std::string res;
    put_u32(res, u32_of(static_cast<std::uint64_t>(dpi) * 65536));
    put_u16(res, 1);
    put_u16(res, 1);
    put_u32(res, u32_of(static_cast<std::uint64_t>(dpi) * 65536));
    put_u16(res, 1);
    put_u16(res, 1);
    std::string block = "8BIM";
    put_u16(block, 1005);
    block += std::string(2, '\0');
    put_u32(block, static_cast<std::uint32_t>(res.size()));
    block += res;
    std::string out;
    put_u32(out, static_cast<std::uint32_t>(block.size()));
    return out + block;
}

}  // namespace

fs::path export_page_psd(const core::Document& episode, const core::Page& page, const fs::path& dest, std::optional<std::int64_t> dpi_given) {
    const std::int64_t dpi = dpi_given && *dpi_given != 0 ? *dpi_given : episode.spec.dpi.truthy() ? core::py_int(episode.spec.dpi) : 600;
    if (dpi < 1 || dpi > 100000) throw core::Error("image_too_large", "the page is too large at this resolution");
    render::RenderOptions options;
    options.mode = "print";
    const Image merged = render::render_page(page, static_cast<int>(dpi), options, &episode).image;
    const Image rgb = merged.mode() == "RGB" ? merged : merged.convert("RGB");
    Records records(rgb.size());
    render::page_layers(page, episode, static_cast<int>(dpi), [&](render::PageLayer&& layer) { records.add(std::move(layer)); });
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    put_u16(body, 3);
    put_u32(body, static_cast<std::uint32_t>(rgb.height()));
    put_u32(body, static_cast<std::uint32_t>(rgb.width()));
    put_u16(body, 8);
    put_u16(body, 3);
    put_u32(body, 0);  // (no colour mode data)
    body += resources(dpi);
    body += records.finish();
    // the merged picture: RLE, every row's count of every channel first
    std::string counts, packed;
    for (const Image& channel : rgb.split()) packbits_rows(channel, counts, packed);
    put_u16(body, 1);
    body += counts;
    body += packed;
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    detail::Output output;
    output.put(dest, body);
    output.commit();
    return dest;
}

std::vector<fs::path> export_psd_pages(const core::Document& episode, const fs::path& dest, std::optional<std::int64_t> dpi) {
    detail::make_dirs(dest);
    std::vector<fs::path> written;
    for (const auto& page : episode.pages) {
        written.push_back(export_page_psd(episode, *page, detail::join(dest, stem(episode) + "_p" + detail::padded(page->index, 3) + ".psd"), dpi));
    }
    return written;
}

fs::path export_psd(const core::Document& episode, const fs::path& dest, std::int64_t dpi, std::int64_t page) {
    for (const auto& p : episode.pages) {
        if (p->index == core::Num(page)) return export_page_psd(episode, *p, dest, dpi);
    }
    throw core::Error("key", "no page " + std::to_string(page));  // (Python: StopIteration)
}

}  // namespace genko::formats
