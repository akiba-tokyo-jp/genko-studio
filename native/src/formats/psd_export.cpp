// PSD export (Python's genko/psd.py, its writer): one file a page with real layers (render::page_layers), each stored
// cut to what it holds and PackBits-compressed, the printed page as the merged picture. Layer names in Unicode (the
// luni block) with an ASCII one beside it. The editable PSD (PSD-01, not Python's; formats/psd_text.hpp) writes each
// placed line as a group of its balloon, its text and its rubies, the text as Photoshop text layers. A page's smart objects
// are Photoshop's placed layers of the files they show (PC3 A3, the user's decision 3: PlacedLayers).

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

#include "core/channels.hpp"
#include "core/error.hpp"
#include "core/masks.hpp"
#include "core/pyconv.hpp"
#include "core/pynum.hpp"
#include "core/pyops.hpp"
#include "core/ruby.hpp"
#include "core/smart.hpp"
#include "formats/file_xmp.hpp"
#include "formats/export.hpp"
#include "formats/output.hpp"
#include "formats/pillow_save.hpp"
#include "formats/psd_adjust.hpp"
#include "formats/psd_text_detail.hpp"
#include "render/channels.hpp"
#include "render/color_canvas.hpp"
#include "render/ink.hpp"
#include "render/ink_canvas.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"
#include "render/png.hpp"
#include "render/psd_smart.hpp"
#include "render/psd_paths.hpp"
#include "render/psd_shapes.hpp"
#include "render/pat.hpp"
#include "core/patterns.hpp"
#include "render/psd_text.hpp"
#include "render/smart.hpp"
#include "render/text/balloons.hpp"
#include "render/text/fonts.hpp"
#include "render/text/lettering.hpp"
#include "storage/asset_store.hpp"

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
// color-model@1: a sample of `depth` bits, big-endian: 8 and 16 rounded from 0..1 (clamped), 32 a float as it is.
void put_sample(std::string& out, double v, int depth) {
    if (depth == 32) put_u32(out, std::bit_cast<std::uint32_t>(static_cast<float>(v)));
    else if (depth == 16) put_u16(out, static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0)));
    else put_u8(out, static_cast<unsigned>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)));
}

// color-model@1: the RGB page of a PSD whose layers are written at its depth (8, 16, 32) in its space (`space`: what is
// drawn in sRGB is converted into it; a paint layer of a profiled page holds its values already).
struct RgbOut {
    int depth = 8;
    render::RgbSpace space;
};

// struct.pack(">I", n): struct.error past 32 bits
std::uint32_t u32_of(std::uint64_t n) {
    if (n > 0xffffffffULL) throw core::Error("value", "'I' format requires 0 <= number <= 4294967295");
    return static_cast<std::uint32_t>(n);
}

// Warp Text (PC3 C-c, the style's native_warp) as the text layer's warp: Photoshop's style by its name ("flag" →
// "warpFlag"), the bend (the style's own 50 when none is given, as the letters are drawn) and the distortions, along or
// across.
void with_warp(render::psd::TextLayer& text, const core::StoryLine& line) {
    const core::Json st = render::text::style_of(line);
    if (!st.is_object() || !st.contains("native_warp") || !st["native_warp"].is_object()) return;
    const core::Json& w = st["native_warp"];
    const auto number = [&](const char* key, double fallback) { return w.contains(key) && w[key].is_number() ? w[key].get<double>() : fallback; };
    std::string style = w.contains("style") && w["style"].is_string() ? w["style"].get<std::string>() : std::string();
    if (style.empty()) return;
    style[0] = static_cast<char>(style[0] - 'a' + 'A');
    text.warp = "warp" + style;
    text.warp_value = number("bend", 50.0);
    text.warp_perspective = number("h_distort", 0.0);
    text.warp_perspective_other = number("v_distort", 0.0);
    text.warp_vertical = w.contains("vertical") && w["vertical"].is_boolean() && w["vertical"].get<bool>();
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
// A tagged block of a layer record (below).
std::string tagged(std::string_view key, std::string payload);

class Records {
public:
    // (`ink`: a CMYK, Lab or grey page's PSD — its layers in the page's inks; `rgb`: an RGB page of a colour model's —
    // its layers at its depth, in its space)
    explicit Records(render::Size size, std::optional<core::PageColor> ink = std::nullopt, std::optional<RgbOut> rgb = std::nullopt)
        : size_(size), ink_(std::move(ink)), rgb_(std::move(rgb)) {}

    // (PC3 A3) a smart object's picture written with the blocks of a placed layer (PlacedLayers::blocks)
    void placed(std::function<std::string(const core::Layer&)> blocks) { placed_ = std::move(blocks); }
    std::string placed_blocks(const render::PageLayer& layer) const {
        return placed_ && layer.smart != nullptr ? placed_(*layer.smart) : std::string();
    }

    // vector@1 (PC3 C-a): the page's paper (mm), a vector mask's vmsk block being in fractions of it, and a layer's vector
    // mask as that block ("" for none: render/psd_paths) — its density and feather noted for the next record's mask data
    // (the user's decision 3: mask_data)
    void paper(double width_mm, double height_mm) { paper_ = {width_mm, height_mm}; }
    std::string vector_block(const std::optional<core::vector::VectorMask>& mask) {
        vector_ = mask;
        if (!mask) return {};
        std::string payload = render::psd_paths::vmsk(*mask, paper_[0], paper_[1]);
        payload.append((4 - payload.size() % 4) % 4, '\0');
        std::string out = "8BIMvmsk";
        put_u32(out, u32_of(payload.size()));
        return out + payload;
    }

    // (PC3 B2, masks@1) the page its layers are of: a layer's mask of masks@1 is written with its flags and parameters
    void masks_of(const core::Page& page) { page_ = &page; }
    // patterns@1 (PC4 E3): the book whose patterns its pattern fills name (their names, unique ids and dpi)
    void patterns_of(const core::Document& book) { book_ = &book; }
    // The next record's mask settings: the page layer's mask, when it has masks@1's (one not used is written too, as
    // Photoshop keeps it, disabled); none for a mask without them (written as before).
    void note_mask(render::PageLayer& layer) {
        settings_.reset();
        if (page_ == nullptr || layer.id.empty()) return;
        for (const core::Layer& own : page_->layers) {
            if (own.id != layer.id || !own.mask || !own.mask->png || own.mask->png->empty() || !core::mask_has_settings(*own.mask)) continue;
            settings_ = *own.mask;
            if (!layer.mask) layer.mask = render::read_png(*own.mask->png).convert("L").resize(size_);
            return;
        }
    }
    void note_mask(const core::Layer& own) {
        settings_.reset();
        if (own.mask && own.mask->enabled && core::mask_has_settings(*own.mask)) settings_ = *own.mask;
    }

    // pen-shapes@1 (PC3 C-b): a shape part's blocks (render/psd_shapes) on this canvas
    std::string shape_blocks(const core::vector::ShapePart& part) const {
        render::psd_shapes::Canvas canvas;
        canvas.width_mm = paper_[0];
        canvas.height_mm = paper_[1];
        canvas.width_px = size_.width;
        canvas.height_px = size_.height;
        canvas.dpi = paper_[0] > 0 ? size_.width / (paper_[0] / 25.4) : 72.0;
        canvas.book = book_;  // (patterns@1, PC4 E3)
        return render::psd_shapes::blocks(part, canvas);
    }
    // patterns@1 (PC4 E3): a pattern fill layer's PtFl block on this canvas
    std::string pattern_blocks(const render::PageLayer& layer) const {
        if (layer.pattern == nullptr) return {};
        render::psd_shapes::Canvas canvas;
        canvas.width_mm = paper_[0];
        canvas.height_mm = paper_[1];
        canvas.width_px = size_.width;
        canvas.height_px = size_.height;
        canvas.dpi = paper_[0] > 0 ? size_.width / (paper_[0] / 25.4) : 72.0;
        canvas.book = book_;
        return tagged("PtFl", render::psd_shapes::pattern_block(*layer.pattern, canvas));
    }

    void add(render::PageLayer&& layer) {
        // (pen-shapes@1, PC3 C-b: a pen layer written as a group — its end below its layers, the group with its settings
        // above them —, its parts as shape layers: their pixels Photoshop's to draw again)
        if (layer.section == "end") {
            group_end();
            return;
        }
        note_mask(layer);  // (PC3 B2: its mask's masks@1 settings — a pen layer's group's too)
        if (layer.section == "group") {
            section(layer.name, 1, blend_key(layer.settings ? layer.blend : std::string("normal")), layer.settings ? layer.opacity : 1.0,
                    layer.mask ? &*layer.mask : nullptr, vector_block(layer.vector_mask));
            return;
        }
        if (layer.shape != nullptr) flags_ = 0x18;  // (bits 3 and 4: its pixel data is not what it shows)
        const std::string blocks = vector_block(layer.vector_mask) + placed_blocks(layer) + (layer.shape != nullptr ? shape_blocks(*layer.shape) : std::string()) +
                                   pattern_blocks(layer);  // (patterns@1, PC4 E3)
        const Image rgba = layer.image.mode() == "RGBA" ? std::move(layer.image) : layer.image.convert("RGBA");
        if (ink_) {  // (its colours by the page's rules)
            add_inks(layer.name, render::ink_picture(rgba, *ink_).values, ink_->space, ink_->depth, layer.settings ? layer.opacity : 1.0,
                     layer.settings ? layer.blend : std::string("normal"), layer.settings && layer.clip, layer.mask ? &*layer.mask : nullptr, blocks);
            return;
        }
        const std::optional<render::Box> found = rgba.getbbox();
        const render::Box box = found ? *found : render::Box{0, 0, 1, 1};  // (an empty layer kept as 1 pixel)
        const double opacity_given = layer.settings ? layer.opacity : 1.0;
        if (rgb_) {
            write_rgb(layer.name, rgba.crop(box), box, opacity_given, blend_key(layer.settings ? layer.blend : std::string("normal")),
                      layer.settings && layer.clip, layer.mask ? &*layer.mask : nullptr, blocks, layer.page_values);
            return;
        }
        write(layer.name, rgba.crop(box), box, opacity_given, blend_key(layer.settings ? layer.blend : std::string("normal")),
              layer.settings && layer.clip, layer.mask ? &*layer.mask : nullptr, blocks);
    }

    // (PSD-01) a layer of a picture that is a part of the page (its top left at `origin`), cut to what it holds, with
    // more tagged blocks after its name (a text layer's TySh, its effects' lfx2)
    void add_part(const std::string& name, const Image& part, render::Point origin, const std::string& blocks) {
        const Image rgba = part.mode() == "RGBA" ? part : part.convert("RGBA");
        const std::optional<render::Box> found = rgba.getbbox();
        const render::Box local = found ? *found : render::Box{0, 0, 1, 1};
        const render::Box box{local.x0 + origin.x, local.y0 + origin.y, local.x1 + origin.x, local.y1 + origin.y};
        if (ink_) {  // (its colours by the page's rules)
            const Image crop = rgba.crop(local);
            write_inks(name, render::ink_picture(crop, *ink_).values, crop.size(), render::Point{box.x0, box.y0}, ink_->space, ink_->depth, 1.0,
                       "normal", false, nullptr, blocks);
            return;
        }
        if (rgb_) {
            write_rgb(name, rgba.crop(local), box, 1.0, "norm", false, nullptr, blocks, false);
            return;
        }
        write(name, rgba.crop(local), box, 1.0, "norm", false, nullptr, blocks);
    }
    // (PSD-01) a group: its end (below its layers) and the group itself (above them), as Photoshop writes them
    void group_end() { section("</Layer group>", 3, "norm"); }
    void group(const std::string& name) { section(name, 1, "pass"); }

    // PC2 (the user's decision 5): a correction layer as an adjustment layer (formats/psd_adjust): no pixels — a channel of
    // nothing for each of the document's and the alpha, as a group's —, its mask at the document's depth (8 bits RLE, 16
    // and 32 raw), its adjustment block after its name.
    void add_adjustment(const PsdAdjustLayer& layer) {
        const int colours = ink_ ? static_cast<int>(core::colour_channels(ink_->space)) : 3;
        const int depth = ink_ ? ink_->depth : rgb_ ? rgb_->depth : 8;
        std::vector<std::pair<int, std::string>> channels;
        for (int id = -1; id < colours; ++id) channels.emplace_back(id, std::string(2, '\0'));
        std::optional<std::string> mask_plane;
        if (layer.mask) {
            const std::string grey = layer.mask->convert("L").resize(size_).tobytes();
            if (depth == 8) {
                mask_plane = packed_plane(grey, size_.width, size_.height, 8);
            } else {
                std::string samples;
                for (const char g : grey) put_sample(samples, static_cast<unsigned char>(g) / 255.0, depth);
                mask_plane = packed_plane(samples, size_.width, size_.height, depth);
            }
        }
        record(layer.name, std::move(channels), render::Box{0, 0, 0, 0}, layer.opacity, blend_key(layer.blend), layer.clip, std::move(mask_plane),
               layer.blocks);
    }

    // (`global`: the global tagged blocks after the global layer mask — PC3 A3: the linked files of placed layers)
    std::string finish(const std::string& global = {}) {
        std::string info;
        put_u16(info, static_cast<std::uint32_t>(static_cast<std::int16_t>(count_)) & 0xffff);
        info += records_;
        info += data_;
        if (info.size() % 2) info += '\0';
        std::string payload;
        put_u32(payload, u32_of(info.size()));
        payload += info;
        put_u32(payload, 0);  // no global layer mask
        payload += global;
        std::string out;
        put_u32(out, u32_of(payload.size()));
        return out + payload;
    }

private:
    void write(const std::string& name, const Image& crop, const render::Box& box, double opacity_given, const std::string& blend, bool clip,
               const Image* mask, const std::string& blocks) {
        const std::vector<Image> bands = crop.split();
        std::vector<std::pair<int, std::string>> channels{{-1, rle(bands[3])}, {0, rle(bands[0])}, {1, rle(bands[1])}, {2, rle(bands[2])}};
        record(name, std::move(channels), box, opacity_given, blend, clip, mask ? std::optional<std::string>(rle(mask->convert("L").resize(size_))) : std::nullopt,
               blocks);
    }

    // color-model@1: a layer of an RGB page of a colour model at its depth: its colours in the page's space (drawn in
    // sRGB: converted, unless `page_values`), its alpha and mask at that depth (8 bits RLE, 16 and 32 raw).
    void write_rgb(const std::string& name, const Image& crop, const render::Box& box, double opacity_given, const std::string& blend, bool clip,
                   const Image* mask, const std::string& blocks, bool page_values) {
        const int depth = rgb_->depth;
        const std::string bytes = crop.tobytes();
        const std::size_t n = std::size_t(crop.width()) * static_cast<std::size_t>(crop.height());
        std::vector<double> rgb(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0;
        }
        if (!page_values && !rgb_->space.is_srgb()) rgb_->space.from_srgb(rgb.data(), n);
        const auto plane = [&](unsigned c) {
            std::string out;
            for (std::size_t i = 0; i < n; ++i) put_sample(out, c == 3 ? static_cast<unsigned char>(bytes[i * 4 + 3]) / 255.0 : rgb[i * 3 + c], depth);
            return out;
        };
        std::vector<std::pair<int, std::string>> channels;
        channels.emplace_back(-1, packed_plane(plane(3), crop.width(), crop.height(), depth));
        for (unsigned c = 0; c < 3; ++c) channels.emplace_back(static_cast<int>(c), packed_plane(plane(c), crop.width(), crop.height(), depth));
        std::optional<std::string> mask_plane;
        if (mask != nullptr) {
            const std::string grey = mask->convert("L").resize(size_).tobytes();
            std::string samples;
            for (const char g : grey) put_sample(samples, static_cast<unsigned char>(g) / 255.0, depth);
            mask_plane = packed_plane(samples, size_.width, size_.height, depth);
        }
        record(name, std::move(channels), box, opacity_given, blend, clip, std::move(mask_plane), blocks);
    }

public:
    // color-mode@1: a layer of a CMYK or Lab page's PSD from its inks over the page (values: per pixel the colour
    // channels then the alpha), cut to what it holds: CMYK stored inverted (0 is full ink), Lab as its bytes; 8 bits
    // RLE, 16 bits raw.
    void add_inks(const std::string& name, const std::vector<double>& values, core::InkSpace space, int depth, double opacity,
                  const std::string& blend, bool clip, const Image* mask, const std::string& blocks = {}) {
        write_inks(name, values, size_, render::Point{0, 0}, space, depth, opacity, blend, clip, mask, blocks);
    }

private:
    // A picture of inks (`values` over `picture`, its top left at `at` on the page) as a layer, cut to what it holds,
    // with more tagged blocks after its name.
    void write_inks(const std::string& name, const std::vector<double>& values, render::Size picture, render::Point at, core::InkSpace space,
                    int depth, double opacity, const std::string& blend, bool clip, const Image* mask, const std::string& blocks) {
        const unsigned n = core::colour_channels(space), all = n + 1;
        render::Box box{picture.width, picture.height, 0, 0};  // (in the picture)
        for (int y = 0; y < picture.height; ++y) {
            for (int x = 0; x < picture.width; ++x) {
                if (values[(std::size_t(y) * picture.width + x) * all + n] <= 0) continue;
                box.x0 = std::min(box.x0, x);
                box.y0 = std::min(box.y0, y);
                box.x1 = std::max(box.x1, x + 1);
                box.y1 = std::max(box.y1, y + 1);
            }
        }
        if (box.x1 <= box.x0) box = render::Box{0, 0, 1, 1};
        const double most = depth == 16 ? 65535.0 : 255.0;
        const auto plane = [&](unsigned channel) {
            std::string bytes;
            for (int y = box.y0; y < box.y1; ++y) {
                for (int x = box.x0; x < box.x1; ++x) {
                    double v = values[(std::size_t(y) * picture.width + x) * all + channel];
                    if (channel < n && space == core::InkSpace::Cmyk) v = 1.0 - v;
                    if (depth == 32) {  // (color-model@1: a grey page's lightness as floats; its alpha 0..1)
                        put_sample(bytes, channel < n ? v : std::clamp(v, 0.0, 1.0), 32);
                        continue;
                    }
                    const auto sample = static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 1.0) * most));
                    if (depth == 16) put_u16(bytes, sample);
                    else put_u8(bytes, sample);
                }
            }
            return bytes;
        };
        std::vector<std::pair<int, std::string>> channels;
        channels.emplace_back(-1, packed_plane(plane(n), box.width(), box.height(), depth));
        for (unsigned c = 0; c < n; ++c) channels.emplace_back(static_cast<int>(c), packed_plane(plane(c), box.width(), box.height(), depth));
        std::optional<std::string> mask_plane;
        if (mask != nullptr) {
            const std::string grey = mask->convert("L").resize(size_).tobytes();
            std::string bytes;
            for (const char g : grey) {
                const unsigned v = static_cast<unsigned char>(g);
                if (depth == 32) put_sample(bytes, v / 255.0, 32);
                else if (depth == 16) put_u16(bytes, v * 257);
                else put_u8(bytes, v);
            }
            mask_plane = packed_plane(bytes, size_.width, size_.height, depth);
        }
        record(name, std::move(channels), render::Box{box.x0 + at.x, box.y0 + at.y, box.x1 + at.x, box.y1 + at.y}, opacity, blend_key(blend), clip,
               std::move(mask_plane), blocks);
    }

public:
    // A plane of samples (rows of `width`, big-endian at 16 bits) as a layer channel's data: 8 bits RLE, 16 raw.
    static std::string packed_plane(const std::string& bytes, int width, int height, int depth) {
        if (depth == 8) return rle(Image::frombytes("L", render::Size{width, height}, bytes));
        std::string out;
        put_u16(out, 0);
        return out + bytes;
    }

private:
    // A feather (mm) in pixels of the canvas, as the mask parameters hold it (a double, big-endian).
    void put_feather(std::string& out, double feather_mm) const {
        const double width_mm = page_ != nullptr ? page_->spec.width_mm.value() : 0.0;
        const double px = width_mm > 0 ? feather_mm * size_.width / width_mm : 0.0;
        std::uint64_t bits = 0;
        std::memcpy(&bits, &px, sizeof bits);
        put_u32(out, static_cast<std::uint32_t>(bits >> 32));
        put_u32(out, static_cast<std::uint32_t>(bits & 0xffffffffu));
    }

    // The next record's mask data (its length first), the settings noted for it used up. With its mask (`picture`: an L
    // picture over the whole canvas, white shows) of masks@1 (PC3 B2, the user's decision 3): Photoshop's flags — bit 0
    // not linked, bit 1 disabled, bit 4 its parameters — and its user density (0..255) and feather (pixels) after them,
    // as PhotoCraft's io writes them; a vector mask's density and feather (vector@1, PC3 C-a, the same decision: the
    // parameters' bits 2 and 3) after those — written alone, with no mask of the layer's, when it has none. Without any of
    // them a mask as before (20 bytes), or none.
    std::string mask_data_of(bool picture) {
        const std::optional<core::Mask> settings = std::exchange(settings_, std::nullopt);
        const std::optional<core::vector::VectorMask> vector = std::exchange(vector_, std::nullopt);
        const bool user = picture && settings && core::mask_has_settings(*settings);
        const bool density = user && settings->density < 1.0, feather = user && settings->feather_mm > 0.0;
        const bool vector_density = vector && vector->density < 1.0, vector_feather = vector && vector->feather_mm > 0.0;
        std::string out;
        if (!user && !vector_density && !vector_feather) {
            if (!picture) {
                put_u32(out, 0);
                return out;
            }
            put_u32(out, 20);
            put_i32(out, 0);
            put_i32(out, 0);
            put_i32(out, size_.height);
            put_i32(out, size_.width);
            out += std::string("\xff\x00\x00\x00", 4);
            return out;
        }
        std::string body;
        put_i32(body, 0);
        put_i32(body, 0);
        put_i32(body, picture ? size_.height : 0);
        put_i32(body, picture ? size_.width : 0);
        put_u8(body, 255);
        unsigned flags = 16u;
        // (a mask without masks@1's settings, written with a vector mask's: fixed to the page, as it is)
        if (picture) flags |= (user && settings->linked ? 0u : 1u) | (user && !settings->enabled ? 2u : 0u);
        put_u8(body, flags);
        put_u8(body, (density ? 1u : 0u) | (feather ? 2u : 0u) | (vector_density ? 4u : 0u) | (vector_feather ? 8u : 0u));
        const auto put_density = [&body](double d) { put_u8(body, static_cast<unsigned>(std::clamp<long>(std::lround(d * 255.0), 0, 255))); };
        if (density) put_density(settings->density);
        if (feather) put_feather(body, settings->feather_mm);
        if (vector_density) put_density(vector->density);
        if (vector_feather) put_feather(body, vector->feather_mm);
        body.append((4 - body.size() % 4) % 4, '\0');
        put_u32(out, u32_of(body.size()));
        return out + body;
    }

    void record(const std::string& name, std::vector<std::pair<int, std::string>> channels, const render::Box& box, double opacity_given,
                const std::string& blend, bool clip, std::optional<std::string> mask_plane, const std::string& blocks) {
        put_i32(records_, box.y0);
        put_i32(records_, box.x0);
        put_i32(records_, box.y1);
        put_i32(records_, box.x1);
        const std::string mask_data = mask_data_of(mask_plane.has_value());
        if (mask_plane) channels.emplace_back(-2, std::move(*mask_plane));
        put_u16(records_, static_cast<std::uint32_t>(channels.size()));
        for (const auto& [id, packed] : channels) {
            put_u16(records_, static_cast<std::uint32_t>(static_cast<std::int16_t>(id)) & 0xffff);
            put_u32(records_, u32_of(packed.size()));
        }
        const auto opacity = static_cast<unsigned>(std::clamp<std::int64_t>(core::py_round_int(255 * opacity_given), 0, 255));
        records_ += "8BIM" + blend;
        put_u8(records_, opacity);
        put_u8(records_, clip ? 1 : 0);
        put_u8(records_, flags_);  // (flags: visible; a shape layer's, pen-shapes@1: 0x18)
        flags_ = 0;
        put_u8(records_, 0);
        std::string extra = mask_data;
        put_u32(extra, 0);
        extra += pascal(name) + unicode_name(name) + blocks;
        put_u32(records_, u32_of(extra.size()));
        records_ += extra;
        for (const auto& [id, packed] : channels) data_ += packed;
        ++count_;
    }

    // A group's record: no pixels (a channel of nothing for each of the document's and the alpha), its section
    // divider (lsct): 1 an open group (with its blend, pass through), 3 the end of one.
    // (pen-shapes@1, PC3 C-b: a pen layer's group with its settings — its opacity, its blend instead of pass through, its
    // mask at the document's depth with its settings, and its vector mask's block)
    void section(const std::string& name, std::uint32_t type, std::string_view blend, double opacity_given = 1.0, const Image* mask = nullptr,
                 const std::string& blocks = {}) {
        for (int i = 0; i < 4; ++i) put_i32(records_, 0);
        const int colours = ink_ ? static_cast<int>(core::colour_channels(ink_->space)) : 3;  // (the document's channels)
        std::optional<std::string> mask_plane;
        if (mask != nullptr) {
            const int depth = ink_ ? ink_->depth : rgb_ ? rgb_->depth : 8;
            const std::string grey = mask->convert("L").resize(size_).tobytes();
            if (depth == 8) {
                mask_plane = packed_plane(grey, size_.width, size_.height, 8);
            } else {
                std::string samples;
                for (const char g : grey) put_sample(samples, static_cast<unsigned char>(g) / 255.0, depth);
                mask_plane = packed_plane(samples, size_.width, size_.height, depth);
            }
        }
        put_u16(records_, static_cast<std::uint32_t>(colours + 1 + (mask_plane ? 1 : 0)));
        for (int id = -1; id < colours; ++id) {
            put_u16(records_, static_cast<std::uint32_t>(static_cast<std::int16_t>(id)) & 0xffff);
            put_u32(records_, 2);
        }
        if (mask_plane) {
            put_u16(records_, static_cast<std::uint32_t>(static_cast<std::int16_t>(-2)) & 0xffff);
            put_u32(records_, u32_of(mask_plane->size()));
        }
        records_ += "8BIM";
        records_ += blend;
        put_u8(records_, static_cast<unsigned>(std::clamp<std::int64_t>(core::py_round_int(255 * opacity_given), 0, 255)));
        put_u8(records_, 0);
        put_u8(records_, 0x18);  // (flags: visible; Photoshop's bits 3 and 4 — its pixels do not show)
        put_u8(records_, 0);
        std::string extra = mask_data_of(mask_plane.has_value());  // (a pen layer's mask settings and vector mask's too)
        put_u32(extra, 0);
        extra += pascal(name) + unicode_name(name) + blocks;
        std::string divider;
        put_u32(divider, type);
        if (type != 3) {
            divider += "8BIM";
            divider += blend;
        }
        extra += "8BIMlsct";
        put_u32(extra, static_cast<std::uint32_t>(divider.size()));
        extra += divider;
        put_u32(records_, u32_of(extra.size()));
        records_ += extra;
        for (int i = 0; i <= colours; ++i) put_u16(data_, 0);  // (each channel raw, with nothing)
        if (mask_plane) data_ += *mask_plane;
        ++count_;
    }

    render::Size size_;
    std::array<double, 2> paper_{0.0, 0.0};
    std::optional<core::PageColor> ink_;
    std::optional<RgbOut> rgb_;
    std::function<std::string(const core::Layer&)> placed_;
    std::string records_;
    std::string data_;
    int count_ = 0;
    const core::Page* page_ = nullptr;     // (PC3 B2: masks_of)
    std::optional<core::Mask> settings_;  // (the next record's mask settings: note_mask)
    std::optional<core::vector::VectorMask> vector_;  // (the next record's vector mask: vector_block)
    unsigned flags_ = 0;  // (the next record's flags: a shape layer's, pen-shapes@1)
    const core::Document* book_ = nullptr;  // (patterns@1, PC4 E3)
};

// A tagged block of a layer record: its key, its length and its payload, padded to 4 bytes.
std::string tagged(std::string_view key, std::string payload);

// --- smart objects as placed layers (PC3 A3) ---------------------------------------------------------------------------

std::string contents_psd_bytes(const core::Document& episode, const core::Page& page, std::int64_t dpi, int depth);

// A file the book keeps for a smart object (a PSD's block, a linked file's record): from the files ops kept, else its
// assets; null when it has none.
core::Bytes kept_bytes(const core::Document& doc, const std::string& ref, std::string_view suffix) {
    if (const auto held = doc.smart_files.find(ref); held != doc.smart_files.end() && held->second) return held->second;
    if (!doc.asset_dir || !storage::AssetStore::is_ref(ref)) return nullptr;
    auto bytes = storage::AssetStore(*doc.asset_dir).get_bytes(ref, suffix, storage::kAssetMaxBytes);
    if (!bytes) return nullptr;
    return std::make_shared<const std::string>(std::move(*bytes));
}

// PC3 A3 (the user's decision 3: a PSD's smart objects are always written; SPEC.md COLOR-05「PSD のスマートオブジェクト」):
// a page's smart objects as Photoshop's placed layers. Each smart object's layer record (its picture, as before) gets a
// SoLd and a PlLd after its name — the id of its contents, where its contents' box lands on the canvas, its warp, its
// contents' size and resolution —, and the file each contents is is written once in the PSD's global lnk2 block (a liFD
// item): a file's own bytes; a page's, a PSD of that page made by this writer (its own smart objects placed layers again,
// at the page's resolution; contents within contents at most core::smart::kDepthMax deep). A smart object read from a PSD
// writes back the block it was read with while that block says where it is (the same place on this canvas), and its
// contents' item as it was read while the contents are unchanged: a PSD read and written again keeps them byte for byte.
class PlacedLayers {
public:
    PlacedLayers(const core::Document& episode, std::int64_t dpi, int depth) : episode_(episode), dpi_(dpi), depth_(depth) {}

    // The layer record's blocks of the smart object `layer` (none for one that cannot be written so: its contents' file
    // not held, or contents too deep).
    std::string blocks(const core::Layer& layer) {
        if (depth_ >= core::smart::kDepthMax) return {};
        const auto id = core::smart::contents_id(layer);
        const core::SmartContents* contents = core::smart::contents_of(episode_, layer);
        if (!id || contents == nullptr || (contents->kind == "page" && !contents->page)) return {};
        render::psd::PlacedOut out;
        if (contents->kind == "page") {
            const std::int64_t res = page_dpi(*contents->page);
            out.resolution = static_cast<double>(res);
            out.width = render::mm_to_px(contents->page->spec.width_mm.value(), static_cast<int>(res));
            out.height = render::mm_to_px(contents->page->spec.height_mm.value(), static_cast<int>(res));
        } else {
            out.resolution = contents->dpi > 0 ? contents->dpi : 72.0;
            out.width = static_cast<double>(contents->width_px);
            out.height = static_cast<double>(contents->height_px);
        }
        // (its contents' id: the one it was read with, unless other contents of this PSD have it)
        std::string uuid = render::psd::uuid_of("genko contents " + *id);
        bool as_read = false;
        if (contents->psd && contents->psd->is_object() && contents->psd->contains("uuid") && (*contents->psd)["uuid"].is_string()) {
            const std::string kept = (*contents->psd)["uuid"].get<std::string>();
            const auto owner = owners_.find(kept);
            if (owner == owners_.end() || owner->second == *id) {
                uuid = kept;
                as_read = true;
            }
        }
        if (!item(*id, *contents, uuid, as_read, out.resolution)) return {};
        out.uuid = uuid;
        out.placed = render::psd::uuid_of("genko placed " + layer.id);
        out.corners = corners(layer, out.width, out.height, out.resolution);
        if (const core::Json* warp = layer.smart ? core::get(*layer.smart, "warp") : nullptr; warp != nullptr && warp->is_object()) {
            out.warp = in_pixels(*warp, out.resolution / 25.4);
        }
        // (the block it was read with, while it says where it is on this canvas)
        if (as_read) {
            const core::Json* psd = core::get(*layer.smart, "psd");
            const core::Json* key = psd != nullptr && psd->is_object() ? core::get(*psd, "block") : nullptr;
            const core::Json* raw = psd != nullptr && psd->is_object() ? core::get(*psd, "raw") : nullptr;
            if (key != nullptr && key->is_string() && raw != nullptr && raw->is_string()) {
                const std::string block = key->get<std::string>();
                if (const core::Bytes bytes = kept_bytes(episode_, raw->get<std::string>(), ".bin")) {
                    const auto read = render::psd::read_placed(block, *bytes);
                    if (read && read->has_corners && read->uuid == uuid && same(read->corners, out.corners)) {
                        return tagged(block, *bytes) + (block == "PlLd" ? tagged("SoLd", render::psd::sold_block(out)) : tagged("PlLd", render::psd::plld_block(out)));
                    }
                }
            }
        }
        return tagged("SoLd", render::psd::sold_block(out)) + tagged("PlLd", render::psd::plld_block(out));
    }

    // The global lnk2 block of the files written (none when there are none).
    std::string global() const {
        if (items_.empty()) return {};
        std::string out = "8BIMlnk2";
        put_u32(out, u32_of(items_.size()));
        return out + items_;
    }

    // The resolution a contents page is written at: its own (none: 600, as a book's).
    static std::int64_t page_dpi(const core::Page& page) { return page.spec.dpi.truthy() ? core::py_int(page.spec.dpi) : 600; }

private:
    // The contents' file in the lnk2 block, once for each id.
    bool item(const std::string& id, const core::SmartContents& contents, const std::string& uuid, bool as_read, double resolution) {
        if (const auto owner = owners_.find(uuid); owner != owners_.end()) return owner->second == id;
        std::string bytes, name = contents.name, type;
        if (contents.kind == "page") {
            bytes = contents_psd_bytes(episode_, *contents.page, static_cast<std::int64_t>(resolution), depth_ + 1);
            if (name.empty()) name = "contents";
            if (!name.ends_with(".psd")) name += ".psd";
            type = "8BPS";
        } else {
            const core::Bytes file = render::smart::file_bytes(episode_, contents);
            if (!file) return false;
            bytes = *file;
            type = render::psd::file_type(contents.suffix == ".psb" ? std::string_view("psb") : std::string_view(contents.format));
            // (the item it was read with, its file's bytes where they were)
            const core::Json* rest = as_read ? core::get(*contents.psd, "item") : nullptr;
            const core::Json* at = as_read ? core::get(*contents.psd, "at") : nullptr;
            if (rest != nullptr && rest->is_string() && at != nullptr && at->is_number_integer()) {
                const core::Bytes kept = kept_bytes(episode_, rest->get<std::string>(), ".bin");
                const auto where = static_cast<std::size_t>(std::max<std::int64_t>(0, at->get<std::int64_t>()));
                if (kept && where <= kept->size()) {
                    items_ += render::psd::framed_item(kept->substr(0, where) + bytes + kept->substr(where));
                    owners_[uuid] = id;
                    return true;
                }
            }
        }
        items_ += render::psd::linked_item(uuid, name, type, bytes);
        owners_[uuid] = id;
        return true;
    }

    // Where the contents' box (w × h pixels at `resolution`) lands on the canvas: through its matrix, or its corners.
    std::array<double, 8> corners(const core::Layer& layer, double w, double h, double resolution) const {
        const double px = static_cast<double>(dpi_) / 25.4;
        std::array<double, 8> out{};
        const core::Json* quad = layer.smart ? core::get(*layer.smart, "perspective") : nullptr;
        if (quad != nullptr && quad->is_array() && quad->size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                out[2 * i] = (*quad)[i][0].get<double>() * px;
                out[2 * i + 1] = (*quad)[i][1].get<double>() * px;
            }
            return out;
        }
        const core::smart::Matrix m = core::smart::matrix_of(layer);
        const double mm = 25.4 / resolution;
        const std::array<std::pair<double, double>, 4> box{{{0, 0}, {w, 0}, {w, h}, {0, h}}};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto [x, y] = m.apply(box[i].first * mm, box[i].second * mm);
            out[2 * i] = x * px;
            out[2 * i + 1] = y * px;
        }
        return out;
    }

    static bool same(const std::array<double, 8>& a, const std::array<double, 8>& b) {
        for (std::size_t i = 0; i < 8; ++i) {
            if (!(std::abs(a[i] - b[i]) <= 0.01)) return false;
        }
        return true;
    }

    // A warp (the contents' mm) in the contents' pixels.
    static core::Json in_pixels(core::Json warp, double k) {
        if (warp.contains("bounds") && warp["bounds"].is_array()) {
            for (core::Json& v : warp["bounds"]) {
                if (v.is_number()) v = v.get<double>() * k;
            }
        }
        if (warp.contains("mesh") && warp["mesh"].is_object() && warp["mesh"].contains("points")) {
            for (core::Json& point : warp["mesh"]["points"]) {
                if (point.is_array() && point.size() == 2 && point[0].is_number() && point[1].is_number())
                    point = core::Json::array({point[0].get<double>() * k, point[1].get<double>() * k});
            }
        }
        return warp;
    }

    const core::Document& episode_;
    std::int64_t dpi_;
    int depth_;
    std::map<std::string, std::string> owners_;  // (a uuid written → the contents' id)
    std::string items_;
};

// PC2 (the user's decision 5): each correction layer of a page, in its place among the layers, as the PSD's adjustment
// layer its kind's codec writes (formats/psd_adjust; one none writes yet: not written, its change in the merged picture as
// in Python's PSD). (What a codec says of a layer: the export's notes, formats::psd_adjust_notes.)
std::function<void(const core::Layer&)> adjustments(Records& records, const core::Page& page, render::Size size, const core::Document& doc) {
    return [&records, &page, size, &doc](const core::Layer& layer) {
        std::vector<std::string> notes;
        if (auto made = psd_adjust_layer(layer, page, size, notes, &doc)) {
            made->blocks += records.vector_block(layer.vector_mask);  // (vector@1, PC3 C-a)
            if (made->mask) records.note_mask(layer);                  // (PC3 B2: its mask's masks@1 settings)
            records.add_adjustment(*made);
        }
    };
}

// A tagged block of a layer record: its key, its length and its payload, padded to 4 bytes.
std::string tagged(std::string_view key, std::string payload) {
    payload.append((4 - payload.size() % 4) % 4, '\0');
    std::string out = "8BIM";
    out += key;
    put_u32(out, u32_of(payload.size()));
    return out + payload;
}

// PSD-01: each placed line of a page as layers of its own (formats/psd_text.hpp): a group with its balloon, its text as
// a text layer and each ruby as one — or, a line using what PSD text cannot keep (the export goes on only when asked to
// draw those as pictures), the layer the image way gives it.
class EditableLines {
public:
    EditableLines(Records& records, const core::Document& episode, int dpi, render::Size size, std::int64_t page, PsdTextReport& report,
                  const core::PageColor* ink = nullptr)
        : records_(records), dpi_(dpi), size_(size), page_(page), report_(report), font_path_(episode.font_path), text_gamma_(episode.text_gamma),
          ink_(ink) {}

    void add(const core::StoryLine& line) {
        const render::text::TextGamma gamma(text_gamma_);  // (color-model@1: the letters drawn as pictures in the book's gamma)
        const std::string words = detail::words_of(line.text, 24);
        std::vector<std::string> features = detail::line_features(line, dpi_, font_path_);
        if (!features.empty()) {  // (asked to: as the image way draws it)
            Image whole = blank();
            render::text::draw_lines(render::text::whole_page(whole), {&line}, dpi_, font_path_, false, nullptr, {}, nullptr);
            render::PageLayer layer;
            layer.name = "台詞 " + words;
            layer.image = std::move(whole);
            records_.add(std::move(layer));
            report_.rasterized.push_back(PsdTextIssue{page_, line.id, words, std::move(features)});
            return;
        }
        detail::font_warnings(line, page_, font_path_, fonts_, nullptr, report_.warnings);
        // (a line of Python's holds its rubies by place where the explicit rule finds them, as ruby_places tells them
        // apart: its text and pixels drawn so — line_features has made sure that sets it as its own lettering does)
        const core::StoryLine twin = detail::by_place(line);
        struct Made {
            std::string name;
            Image picture;
            render::Point origin;
            std::string blocks;
        };
        std::vector<Made> made;
        Image balloon = drawn(twin, render::text::LetterParts{true, false, false, true});
        if (const auto box = balloon.getbbox()) made.push_back(Made{"フキダシ", balloon.crop(*box), {box->x0, box->y0}, {}});
        balloon = Image();
        const render::text::EditableLine editable = render::text::editable_line(twin, dpi_, font_path_);
        const render::text::SetLetters& set = editable.lettering->set;
        const std::array<double, 6> m = editable.to_page;
        std::string effects;
        if (set.halo) {
            const render::text::Rgb& rgb = set.halo->second;
            render::psd::Stroke halo;
            halo.size = static_cast<double>(set.halo->first);
            halo.rgb = {colour(rgb, 0), colour(rgb, 1), colour(rgb, 2)};
            if (ink_ != nullptr) {  // (color-mode@1: its colour in the document's mode, by the page's rules)
                const std::vector<double> ink = render::ink::ink_of(*ink_, halo.rgb);
                if (ink_->space == core::InkSpace::Cmyk) halo.cmyk = std::array<double, 4>{ink[0], ink[1], ink[2], ink[3]};
                else halo.lab = std::array<double, 3>{ink[0], ink[1], ink[2]};
            }
            effects = tagged("lfx2", render::psd::lfx2(halo));
        }
        render::psd::TextLayer body = in_inks(body_text(line, set, m));
        with_warp(body, line);  // (Warp Text: the text layer's own warp)
        if (!body.text.empty()) {
            Image letters = drawn(twin, render::text::LetterParts{false, true, false, false});
            const auto box = letters.getbbox();
            made.push_back(Made{"本文 #" + line.id, box ? letters.crop(*box) : letters.crop(render::Box{0, 0, 1, 1}),
                                box ? render::Point{box->x0, box->y0} : render::Point{0, 0}, tagged("TySh", render::psd::tysh(body)) + effects});
            ++report_.text_layers;
        }
        if (!set.rubies.empty()) {
            Image rubies = drawn(twin, render::text::LetterParts{false, false, true, false});
            for (std::size_t k = 0; k < set.rubies.size(); ++k) {
                const render::text::RubyDrawn& place = set.rubies[k];
                std::size_t parts = 0, nth = 0;
                for (std::size_t j = 0; j < set.rubies.size(); ++j) {
                    if (set.rubies[j].span != place.span) continue;
                    ++parts;
                    if (j <= k) nth = parts;
                }
                const std::string name = "ルビ「" + render::text::utf8(place.reading) + "」 #" + line.id + "-r" + std::to_string(place.span) +
                                         (parts > 1 ? "." + std::to_string(nth) : std::string());
                // its pixels: the reading's box on the page (and a little round it), taken from the rubies' picture
                const render::Box box = on_page(m, place.ruby, 3);
                Image piece = rubies.crop(box);
                rubies.paste(Image::create("RGBA", piece.size(), render::Ink{0, 0, 0, 0}), render::Point{box.x0, box.y0});
                made.push_back(Made{name, std::move(piece), {box.x0, box.y0}, tagged("TySh", render::psd::tysh(in_inks(ruby_text(set, place, m)))) + effects});
                ++report_.ruby_layers;
            }
        }
        if (made.empty()) return;
        records_.group_end();
        for (const Made& layer : made) records_.add_part(layer.name, layer.picture, layer.origin, layer.blocks);
        records_.group("台詞 " + words);
    }

private:
    Image blank() const { return Image::create("RGBA", size_, render::Ink{0, 0, 0, 0}); }

    // (color-mode@1) a text's colours as a CMYK document's inks by the page's rules (FillColor Type 2); a Lab document's
    // stay RGB, the colour the text engine takes and the page's rules stand for.
    render::psd::TextLayer in_inks(render::psd::TextLayer text) const {
        if (ink_ == nullptr || ink_->space != core::InkSpace::Cmyk) return text;
        for (render::psd::StyleRun& run : text.runs) {
            const std::vector<double> ink = render::ink::ink_of(*ink_, run.style.fill);
            run.style.fill_cmyk = std::array<double, 4>{ink[0], ink[1], ink[2], ink[3]};
        }
        return text;
    }

    Image drawn(const core::StoryLine& line, const render::text::LetterParts& parts) const {
        Image out = blank();
        render::text::draw_lines(render::text::whole_page(out), {&line}, dpi_, font_path_, false, nullptr, {}, nullptr, &parts);
        return out;
    }

    static int colour(const render::text::Rgb& rgb, std::size_t i) {
        return i < rgb.size() ? static_cast<int>(std::clamp<std::int64_t>(rgb[i], 0, 255)) : 0;
    }

    // The box a set box (the lettering's own pixels) covers on the page through `m`, `margin` pixels round it, within
    // the page.
    render::Box on_page(const std::array<double, 6>& m, const render::BoxF& b, int margin) const {
        double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool first = true;
        for (const auto& [x, y] : {std::pair{b.x0, b.y0}, std::pair{b.x1, b.y0}, std::pair{b.x0, b.y1}, std::pair{b.x1, b.y1}}) {
            const double px = m[0] * x + m[2] * y + m[4];
            const double py = m[1] * x + m[3] * y + m[5];
            x0 = first ? px : std::min(x0, px);
            y0 = first ? py : std::min(y0, py);
            x1 = first ? px : std::max(x1, px);
            y1 = first ? py : std::max(y1, py);
            first = false;
        }
        const auto clampx = [&](double v) { return static_cast<int>(std::clamp<double>(v, 0, size_.width)); };
        const auto clampy = [&](double v) { return static_cast<int>(std::clamp<double>(v, 0, size_.height)); };
        return render::Box{clampx(std::floor(x0) - margin), clampy(std::floor(y0) - margin), clampx(std::ceil(x1) + margin),
                           clampy(std::ceil(y1) + margin)};
    }

    // The layer's transform: the set letters' map to the page from the text's anchor (a set point) — without the lean
    // of style.italic, which the runs carry as a slanted face (FauxItalic) instead.
    static std::array<double, 6> transform(const render::text::SetLetters& set, const std::array<double, 6>& m, double ax, double ay) {
        std::array<double, 6> t{m[0], m[1], m[2], m[3], m[0] * ax + m[2] * ay + m[4], m[1] * ax + m[3] * ay + m[5]};
        if (set.italic) {
            const double k = core::py_tan(12.0 * (core::kPi / 180.0));
            const double f = set.to_image[0] != 0.0 ? set.to_image[0] : 1.0;
            if (set.vertical) {
                t[0] = m[0] + m[2] * k * f;
                t[1] = m[1] + m[3] * k * f;
            } else {
                t[2] = m[2] + m[0] * k / f;
                t[3] = m[3] + m[1] * k / f;
            }
        }
        return t;
    }

    std::string font_name(const std::string& path) {
        std::string name = detail::font_name(fonts_, path);
        if (std::find(report_.fonts.begin(), report_.fonts.end(), name) == report_.fonts.end()) report_.fonts.push_back(name);
        return name;
    }

    // Characters as the PSD's text holds them: a control character (U+0000–U+001F, U+007F; the lettering sets one as a
    // character of its row) is a space — U+000D would be a paragraph break and U+0003 a forced line break there.
    static std::u16string engine_text(std::u32string_view text) {
        std::u32string kept(text);
        for (char32_t& c : kept) {
            if (c < 0x20 || c == 0x7f) c = U' ';
        }
        return render::psd::utf16(render::text::utf8(kept));
    }

    // The runs of characters in one style, from each character's.
    static void runs_of(const std::vector<render::psd::TextStyle>& styles, render::psd::TextLayer& layer) {
        for (const render::psd::TextStyle& style : styles) {
            if (!layer.runs.empty() && layer.runs.back().style == style) {
                ++layer.runs.back().length;
            } else {
                layer.runs.push_back(render::psd::StyleRun{1, style});
            }
        }
    }

    render::psd::TextLayer body_text(const core::StoryLine& line, const render::text::SetLetters& set, const std::array<double, 6>& m) {
        render::psd::TextLayer out;
        out.vertical = set.vertical;
        const core::Json st = render::text::style_of(line);
        const auto number = [&](const char* key, double fallback) {
            const auto it = st.find(key);
            return it != st.end() && core::py_truthy(*it) ? core::to_float(*it) : fallback;
        };
        const auto em = static_cast<double>(set.em);
        // the space after each character (px): down, Python's whole pixels and never less than nothing
        const double tracking = number("tracking", 0.0);
        const double gap = set.vertical ? static_cast<double>(std::max<std::int64_t>(0, core::py_round_int(em * tracking))) : em * tracking;
        const auto& rows = set.rows;
        // the distance of each row (column) from the one before: baselines down the page, columns' middles to the left
        const auto pitch = [&](std::size_t r) {
            if (rows.size() < 2) {
                return set.vertical ? em * (1.0 + number("leading", 0.4)) : em * (1.15 + number("leading", 0.4));
            }
            const std::size_t a = r == 0 ? 0 : r - 1;
            const std::size_t b = r == 0 ? 1 : r;
            return set.vertical ? rows[a].axis - rows[b].axis : rows[b].axis - rows[a].axis;
        };
        std::vector<render::psd::TextStyle> styles;
        render::psd::TextStyle last;
        bool have_last = false;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const double leading = pitch(r);
            std::size_t start = out.text.size();
            for (const render::text::SetChar& ch : rows[r].chars) {
                render::psd::TextStyle style;
                style.font = font_name(ch.font);
                style.size = static_cast<double>(ch.size);
                style.faux_bold = ch.weight > 0;
                style.faux_italic = set.italic;
                style.fill = {colour(ch.rgb, 0), colour(ch.rgb, 1), colour(ch.rgb, 2)};
                style.tracking = ch.size > 0 ? gap / static_cast<double>(ch.size) * 1000.0 : 0.0;
                style.leading = leading;
                // (down: half-width letters laid on their side 2, across in their cell 3, every other cell upright 1 —
                // a lone letter, digit or ! is upright, as the lettering keeps it; across: the text engine's default)
                style.baseline_direction = !set.vertical ? 2 : ch.sideways ? 2 : ch.tcy ? 3 : 1;
                style.run_alignment = 2;
                const std::u16string text = engine_text(ch.text);
                out.text += text;
                styles.insert(styles.end(), text.size(), style);
                last = style;
                have_last = true;
            }
            if (!have_last) {  // (an empty first row: the line's own size and face)
                last.font = font_name(set.face.other);
                last.size = em;
                last.faux_italic = set.italic;
            }
            last.leading = leading;
            // its paragraph: its characters and the line break after (the text engine's own at the end)
            out.paragraphs.push_back(render::psd::ParagraphRun{out.text.size() - start + 1, 0});
            if (r + 1 < rows.size()) out.text.push_back(u'\r');
            styles.push_back(last);
        }
        if (out.text.empty()) return out;
        runs_of(styles, out);
        // the anchor, where the text is set from (point text), or the box it fills (justified: area text)
        const std::string& align = set.align;
        int justification = 0;
        double ax = 0.0, ay = 0.0;
        const render::BoxF& area = set.area;
        if (align == "justify") {
            justification = set.vertical ? 6 : 3;
            ax = area.x0;
            ay = area.y0;
            out.box = std::array<double, 4>{0.0, 0.0, area.x1 - area.x0, area.y1 - area.y0};
        } else if (set.vertical) {
            justification = align == "center" ? 2 : align == "bottom" ? 1 : 0;
            ax = rows.front().axis;
            ay = justification == 2 ? (area.y0 + area.y1) / 2 : justification == 1 ? area.y1 : area.y0;
        } else {
            justification = align == "center" ? 2 : align == "right" ? 1 : 0;
            ay = rows.front().axis;
            const render::text::SetRow* first = &rows.front();
            for (const auto& row : rows) {
                if (!row.chars.empty()) {
                    first = &row;
                    break;
                }
            }
            if (justification == 0) {
                ax = first->start;
                for (const auto& row : rows) {
                    if (!row.chars.empty()) ax = std::min(ax, row.start);
                }
            } else if (justification == 1) {
                ax = first->end;
                for (const auto& row : rows) {
                    if (!row.chars.empty()) ax = std::max(ax, row.end);
                }
            } else {
                ax = (first->start + first->end) / 2;
            }
        }
        for (render::psd::ParagraphRun& p : out.paragraphs) p.justification = justification;
        out.bounds = {area.x0 - ax, area.y0 - ay, area.x1 - ax, area.y1 - ay};
        out.transform = transform(set, m, ax, ay);
        return out;
    }

    render::psd::TextLayer ruby_text(const render::text::SetLetters& set, const render::text::RubyDrawn& place, const std::array<double, 6>& m) {
        render::psd::TextLayer out;
        out.vertical = place.vertical;
        std::vector<render::psd::TextStyle> styles;
        const std::vector<std::size_t> letters = core::ruby::bounds(place.reading);  // (its graphemes, as the letters set)
        for (std::size_t i = 0; i + 1 < letters.size(); ++i) {
            render::psd::TextStyle style;
            style.font = font_name(i < place.fonts.size() ? place.fonts[i] : place.face.other);
            style.size = static_cast<double>(place.size);
            style.faux_italic = set.italic;
            style.fill = {colour(place.fill, 0), colour(place.fill, 1), colour(place.fill, 2)};
            style.tracking = place.size > 0 ? place.spacing / static_cast<double>(place.size) * 1000.0 : 0.0;
            style.leading = static_cast<double>(place.size);
            const std::u16string text = engine_text(place.reading.substr(letters[i], letters[i + 1] - letters[i]));
            out.text += text;
            styles.insert(styles.end(), text.size(), style);
        }
        if (styles.empty()) return out;
        styles.push_back(styles.back());
        runs_of(styles, out);
        out.paragraphs.push_back(render::psd::ParagraphRun{out.text.size() + 1, 0});
        // set from its first letter: down, the middle of its em square's top; across, where the font draws it from (its
        // baseline)
        const render::PointD first = place.letters.empty() ? render::PointD{place.ruby.x0, place.ruby.y0} : place.letters.front();
        double ax = first.x, ay = first.y;
        if (place.vertical) {
            ax += static_cast<double>(place.size) / 2;
        } else {
            const std::u32string one = place.reading.substr(0, letters.size() > 1 ? letters[1] : place.reading.size());
            ay += fonts_.font(place.face, place.size, one).getmetrics().first;
        }
        out.bounds = {place.ruby.x0 - ax, place.ruby.y0 - ay, place.ruby.x1 - ax, place.ruby.y1 - ay};
        out.transform = transform(set, m, ax, ay);
        return out;
    }

    Records& records_;
    int dpi_;
    render::Size size_;
    std::int64_t page_;
    PsdTextReport& report_;
    std::optional<std::string> font_path_;
    std::optional<double> text_gamma_;  // (the book's)
    const core::PageColor* ink_;  // (a CMYK or Lab page's: its colours)
    render::text::Fonts fonts_;
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

// The resolution a PSD is written at: the one asked for (nothing or 0: the book's, else 600).
std::int64_t psd_dpi(const core::Document& episode, std::optional<std::int64_t> dpi_given) {
    return dpi_given && *dpi_given != 0 ? *dpi_given : episode.spec.dpi.truthy() ? core::py_int(episode.spec.dpi) : 600;
}

// color-mode@1: the ICC profile as image resource 1039 (after the resolution), the resources' length again.
std::string resources_with_icc(std::int64_t dpi, std::string_view icc) {
    std::string out = resources(dpi);
    if (icc.empty()) return out;
    std::string block = out.substr(4);
    block += "8BIM";
    put_u16(block, 1039);
    block += std::string(2, '\0');
    put_u32(block, u32_of(icc.size()));
    block += icc;
    if (icc.size() % 2) block += '\0';
    std::string sized;
    put_u32(sized, u32_of(block.size()));
    return sized + block;
}

// channels@1 (SPEC.md COLOR-03): a page's alpha and spot channels written as the PSD's extra channels, after its colour
// planes in the panel's order, with their names (1006 in Pascal strings, 1045 in UTF-16) and how they show (1077,
// DisplayInfo: version 1, then for each 13 bytes — an RGB colour of four 16-bit components, its opacity, a spot's
// solidity, in percent, and 0 selected areas / 1 masked areas / 2 spot: PhotoCraft's io/channel_map.rs, without the
// padding byte it writes after the kind, which 1007's records have and 1077's do not: GIMP and psd-tools read 13 a
// record, and psd-tools refuses the file otherwise). Their samples are the grey
// Photoshop shows the channel as, its colour where it is dark (as a CMYK plate is stored, 0 the full ink — this
// writer's CMYK planes, Adobe's colour structure, Pillow's and psd-tools' readers —, and as PhotoCraft's own
// multichannel_map.rs writes spot planes): a spot channel max − density (0 the full ink), an alpha channel marking the
// selected areas max − density (0 selected), one marking the masked areas its density (max selected, as GIMP writes its
// channels with kind 1). (PhotoCraft writes a spot and a selected-areas channel of an RGB or CMYK document as its density:
// the other way round.)
std::string image_resource(unsigned id, std::string data) {
    std::string block = "8BIM";
    put_u16(block, id);
    block += std::string(2, '\0');
    put_u32(block, u32_of(data.size()));
    if (data.size() % 2) data += '\0';
    return block + data;
}

std::string utf16_of(const std::string& name) {
    const std::string luni = unicode_name(name);  // ("8BIMluni", its length, the count of units and the units, padded)
    const std::uint32_t units = (static_cast<unsigned char>(luni[12]) << 24) | (static_cast<unsigned char>(luni[13]) << 16) |
                                (static_cast<unsigned char>(luni[14]) << 8) | static_cast<unsigned char>(luni[15]);
    return luni.substr(16, std::size_t(units) * 2);
}

// masks@1 (PC3 B1, SPEC.md COLOR-06「クイックマスク」): the channels in the order they are written — a page's Quick Mask
// last, as Photoshop writes a document saved in Quick Mask mode — and that channel named by image resource 1022 (its
// number among the file's channels, u16, then 0: not "initially empty"), PhotoCraft's io/src/psd_export.rs:619-662. A page
// without a Quick Mask: its channels in their order, no 1022 (the same bytes as before).
std::vector<core::Channel> psd_order(const std::vector<core::Channel>& channels) {
    std::vector<core::Channel> out;
    for (const bool quick : {false, true})
        for (const core::Channel& channel : channels)
            if (channel.quick_mask == quick) out.push_back(channel);
    return out;
}

// vector@1 (PC3 C-a): the page's path resources (its saved paths, work path, clipping path: render/psd_paths) after the
// resources, the resources' length again.
std::string with_paths(const std::string& sized_resources, const core::Page& page) {
    const std::string paths = render::psd_paths::resources(page);
    if (paths.empty()) return sized_resources;
    const std::string blocks = sized_resources.substr(4) + paths;
    std::string out;
    put_u32(out, u32_of(blocks.size()));
    return out + blocks;
}

// `first`: the file's channels before the page's (its colour planes; 0 when `page`'s list holds them, a multichannel
// page's plates).
std::string with_channels(const std::string& sized_resources, const core::Page& page, unsigned first) {
    if (page.channels.empty()) return with_paths(sized_resources, page);
    const std::vector<core::Channel> ordered = psd_order(page.channels);
    std::string names, unicode, display;
    put_u32(display, 1);
    for (const core::Channel& channel : ordered) {
        std::string ascii;
        for (const char c : channel.name) ascii += static_cast<unsigned char>(c) < 0x80 && static_cast<unsigned char>(c) >= 0x20 ? c : '?';
        if (ascii.size() > 255) ascii.resize(255);
        names += static_cast<char>(ascii.size());
        names += ascii;
        const std::string units = utf16_of(channel.name);
        put_u32(unicode, static_cast<std::uint32_t>(units.size() / 2 + 1));
        unicode += units;
        put_u16(unicode, 0);
        const bool spot = channel.kind == core::ChannelKind::Spot;
        put_u16(display, 0);  // (RGB)
        for (const int c : channel.color) put_u16(display, static_cast<std::uint32_t>(c) * 257);
        put_u16(display, 0);
        put_u16(display, static_cast<std::uint32_t>(std::lround(std::clamp(spot ? channel.solidity : channel.opacity, 0.0, 1.0) * 100.0)));
        put_u8(display, spot ? 2 : channel.indicates == "selected" ? 0 : 1);  // (13 bytes: 1077 has no padding byte, unlike 1007)
    }
    std::string blocks = sized_resources.substr(4);
    blocks += image_resource(1006, names);
    blocks += image_resource(1045, unicode);
    blocks += image_resource(1077, display);
    if (ordered.back().quick_mask) {
        std::string info;
        put_u16(info, first + static_cast<unsigned>(ordered.size()) - 1);
        put_u8(info, 0);
        blocks += image_resource(1022, info);
    }
    blocks += render::psd_paths::resources(page);  // (vector@1, PC3 C-a)
    std::string out;
    put_u32(out, u32_of(blocks.size()));
    return out + blocks;
}

// A channel's samples over the merged picture (`size`), at `depth` (8 and 16 rounded, 32 floats), big-endian: its
// density, or 1 − its density for a spot and a channel marking the selected areas (dark where its colour shows).
std::string channel_samples(const core::Page&, const core::Channel& channel, render::Size size, int depth) {
    const render::channels::Density density(channel);
    const bool dark = channel.kind == core::ChannelKind::Spot || channel.indicates == "selected";
    std::string out;
    out.reserve(std::size_t(size.width) * size.height * std::size_t(depth / 8));
    for (int y = 0; y < size.height; ++y) {
        for (int x = 0; x < size.width; ++x) {
            const double v = std::clamp(double(density.at((x + 0.5) / size.width, (y + 0.5) / size.height)), 0.0, 1.0);
            put_sample(out, dark ? 1.0 - v : v, depth);
        }
    }
    return out;
}

// The merged picture's extra channels after its colour planes: 8 bits RLE (their rows' counts after the colours', their
// packed rows after theirs), 16 and 32 raw.
void put_channels_rle(const core::Page& page, render::Size size, std::string& counts, std::string& packed) {
    for (const core::Channel& channel : psd_order(page.channels))
        packbits_rows(Image::frombytes("L", size, channel_samples(page, channel, size, 8)), counts, packed);
}

// The render's options for the merged picture: its spots left out (they are channels of their own).
render::RenderOptions merged_options(const core::Page& page) {
    render::RenderOptions options;
    options.mode = "print";
    if (!page.channels.empty()) options.spots = std::vector<std::string>{};
    return options;
}

// color-mode@1 (SPEC.md COLOR-02): a CMYK or Lab page's PSD in its own colour mode (4 or 9) and depth, every layer and
// the merged picture as its inks (a paint layer's own; the others' colours by the page's rules), a CMYK page's profile
// embedded. The editable text of PSD-01 too: its layers' pixels in inks, a CMYK page's text colours as inks.
// color-model@1 (COLOR-03): a grey page's in Grayscale (mode 1), its lightness, its profile embedded; 32 bits as floats
// for grey — a CMYK or Lab page of 32 bits at 16 (Photoshop has no 32-bit CMYK or Lab: said in the export's notes).
std::string page_psd_inks(const core::Document& episode, const core::Page& page, std::int64_t dpi, const PsdText& text, PsdTextReport* report,
                          int depth) {
    core::PageColor color = *page.color;
    if (color.depth == 32 && color.space != core::InkSpace::Gray) color.depth = 16;  // (the depth written)
    const bool grey = color.space == core::InkSpace::Gray;
    render::RenderOptions options = merged_options(page);
    options.inks = true;
    const render::RenderResult made = render::render_page(page, static_cast<int>(dpi), options, &episode);
    std::optional<core::InkPixels> halved;  // (a 32-bit CMYK or Lab page's at 16)
    if (made.inks->depth != color.depth) halved = detail::at_16(*made.inks);
    const core::InkPixels& merged = halved ? *halved : *made.inks;
    const render::Size size{static_cast<int>(merged.width), static_cast<int>(merged.height)};
    Records records(size, color);
    records.masks_of(page);                    // (PC3 B2)
    records.patterns_of(episode);  // (patterns@1, PC4 E3)
    PlacedLayers placed(episode, dpi, depth);  // (PC3 A3)
    records.placed([&placed](const core::Layer& layer) { return placed.blocks(layer); });
    records.paper(page.spec.width_mm.value(), page.spec.height_mm.value());
    const auto each = [&](render::PageLayer&& layer) {
        if (!layer.section.empty() || layer.shape != nullptr) {  // (pen-shapes@1: a pen layer's group and its shape layers)
            records.add(std::move(layer));
            return;
        }
        records.note_mask(layer);  // (PC3 B2: its mask's masks@1 settings)
        const std::vector<double> values = layer.inks ? *layer.inks : render::ink_picture(layer.image, color).values;
        records.add_inks(layer.name, values, color.space, color.depth, layer.settings ? layer.opacity : 1.0,
                         layer.settings ? layer.blend : std::string("normal"), layer.settings && layer.clip, layer.mask ? &*layer.mask : nullptr,
                         records.vector_block(layer.vector_mask) + records.placed_blocks(layer));
    };
    const std::function<void(const core::Layer&)> adjust_layers = adjustments(records, page, size, episode);
    if (text.editable) {  // (PSD-01: each placed line's layers made here)
        PsdTextReport own;
        EditableLines lines(records, episode, static_cast<int>(dpi), size, core::py_int(page.index.json()), report != nullptr ? *report : own, &color);
        const std::function<void(const core::StoryLine&)> line_layers = [&](const core::StoryLine& line) { lines.add(line); };
        render::page_layers(page, episode, static_cast<int>(dpi), each, &line_layers, &adjust_layers, true);
    } else {
        render::page_layers(page, episode, static_cast<int>(dpi), each, nullptr, &adjust_layers, true);
    }
    const unsigned n = core::colour_channels(color.space);
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    put_u16(body, n + static_cast<unsigned>(page.channels.size()));
    put_u32(body, merged.height);
    put_u32(body, merged.width);
    put_u16(body, static_cast<std::uint32_t>(color.depth));
    put_u16(body, color.space == core::InkSpace::Cmyk ? 4 : grey ? 1 : 9);
    put_u32(body, 0);  // (no colour mode data)
    body += with_channels(resources_with_icc(dpi, color.space != core::InkSpace::Lab && color.icc ? std::string_view(*color.icc) : std::string_view()),
                          page, n);
    body += records.finish(placed.global() + render::pat::global_block(episode, core::patterns_on(page), color.depth));  // (PC4 E3: Patt)
    // the merged picture: each colour plane (CMYK inverted); 8 bits RLE (every row's count first), 16 and 32 raw
    const unsigned all = merged.channels();
    const auto plane = [&](unsigned c) {
        std::string bytes;
        for (std::size_t i = 0; i < merged.pixel_count(); ++i) {
            if (color.depth == 32) {
                put_sample(bytes, merged.floats[i * all + c], 32);
                continue;
            }
            std::uint32_t v = merged.samples[i * all + c];
            if (color.space == core::InkSpace::Cmyk) v = merged.max() - v;
            if (color.depth == 16) put_u16(bytes, v);
            else put_u8(bytes, v);
        }
        return bytes;
    };
    if (color.depth == 8) {
        std::string counts, packed;
        for (unsigned c = 0; c < n; ++c) packbits_rows(Image::frombytes("L", size, plane(c)), counts, packed);
        put_channels_rle(page, size, counts, packed);
        put_u16(body, 1);
        body += counts;
        body += packed;
    } else {
        put_u16(body, 0);
        for (unsigned c = 0; c < n; ++c) body += plane(c);
        for (const core::Channel& channel : psd_order(page.channels)) body += channel_samples(page, channel, size, color.depth);
    }
    return body;
}

// color-model@1 (COLOR-03): an RGB page of a colour model's PSD in RGB (mode 3) at its depth (8, 16, 32 as floats), its
// profile (or sRGB's) embedded: its layers in its values (what is drawn in sRGB converted into its space), the merged
// picture its values (render_page's precise: what is drawn over the layers in 8 bits taken into them).
std::string page_psd_rgb(const core::Document& episode, const core::Page& page, std::int64_t dpi, const PsdText& text, PsdTextReport* report,
                         int depth_of_contents) {
    const core::RgbMode& mode = *page.rgb;
    const int depth = mode.depth;
    const render::RenderOptions options = merged_options(page);
    const detail::PageValues made = detail::page_values(page, episode, static_cast<int>(dpi), options);
    const core::ColorRasterView merged(made.values);
    const render::Size size{static_cast<int>(merged.width()), static_cast<int>(merged.height())};
    Records records(size, std::nullopt, RgbOut{depth, render::RgbSpace::of(page)});
    records.masks_of(page);                                 // (PC3 B2)
    records.patterns_of(episode);  // (patterns@1, PC4 E3)
    PlacedLayers placed(episode, dpi, depth_of_contents);  // (PC3 A3)
    records.placed([&placed](const core::Layer& layer) { return placed.blocks(layer); });
    records.paper(page.spec.width_mm.value(), page.spec.height_mm.value());
    const auto each = [&](render::PageLayer&& layer) { records.add(std::move(layer)); };
    const std::function<void(const core::Layer&)> adjust_layers = adjustments(records, page, size, episode);
    if (text.editable) {  // (PSD-01: each placed line's layers made here)
        PsdTextReport own;
        EditableLines lines(records, episode, static_cast<int>(dpi), size, core::py_int(page.index.json()), report != nullptr ? *report : own);
        const std::function<void(const core::StoryLine&)> line_layers = [&](const core::StoryLine& line) { lines.add(line); };
        render::page_layers(page, episode, static_cast<int>(dpi), each, &line_layers, &adjust_layers, true);
    } else {
        render::page_layers(page, episode, static_cast<int>(dpi), each, nullptr, &adjust_layers, true);
    }
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    put_u16(body, 3 + static_cast<std::uint32_t>(page.channels.size()));
    put_u32(body, merged.height());
    put_u32(body, merged.width());
    put_u16(body, static_cast<std::uint32_t>(depth));
    put_u16(body, 3);
    put_u32(body, 0);  // (no colour mode data)
    body += with_channels(resources_with_icc(dpi, detail::rgb_icc(mode)), page, 3);
    body += records.finish(placed.global() + render::pat::global_block(episode, core::patterns_on(page), depth));  // (PC4 E3: Patt)
    // the merged picture: 8 bits RLE (every row's count first), 16 and 32 raw
    const std::size_t pixels = std::size_t(merged.width()) * merged.height();
    const auto plane = [&](unsigned c) {
        std::string bytes;
        for (std::size_t i = 0; i < pixels; ++i) put_sample(bytes, merged.pixel(i)[c], depth);
        return bytes;
    };
    if (depth == 8) {
        std::string counts, packed;
        for (unsigned c = 0; c < 3; ++c) packbits_rows(Image::frombytes("L", size, plane(c)), counts, packed);
        put_channels_rle(page, size, counts, packed);
        put_u16(body, 1);
        body += counts;
        body += packed;
    } else {
        put_u16(body, 0);
        for (unsigned c = 0; c < 3; ++c) body += plane(c);
        for (const core::Channel& channel : psd_order(page.channels)) body += channel_samples(page, channel, size, depth);
    }
    return body;
}

// color-model@1 (PC1d, SPEC.md COLOR-03): a page of Image › Mode's own as Photoshop holds that mode — the merged picture
// only (Bitmap, Indexed Color and Multichannel documents have no layers: said in the export's notes): a bitmap page in
// Bitmap (mode 0, 1 bit a pixel, 1 black), an indexed page in Indexed Color (mode 2, its table as the colour mode data —
// 256 reds, greens, blues —, 1046 its count, 1047 its transparent index), a multichannel page in Multichannel (mode 7,
// each plate a channel dark where its ink is, as a spot channel: max − density; names in 1006 and 1045, colours and
// solidity in 1077 as spots), its alpha and spot channels after them. A bitmap or indexed page's channels are not
// written (Photoshop has none in those modes: said in the notes); a multichannel page of 32 bits at 16.
std::string page_psd_flat(const core::Document& episode, const core::Page& page, std::int64_t dpi) {
    render::RenderOptions options = merged_options(page);
    const bool indexed = !page.color;
    const bool bitmap = page.color && page.color->bitmap;
    options.inks = !indexed;
    const render::RenderResult made = render::render_page(page, static_cast<int>(dpi), options, &episode);
    const render::Size size = made.image.size();
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    if (bitmap) {
        const core::InkPixels& merged = *made.inks;
        put_u16(body, 1);
        put_u32(body, merged.height);
        put_u32(body, merged.width);
        put_u16(body, 1);
        put_u16(body, 0);
        put_u32(body, 0);  // (no colour mode data)
        body += with_paths(resources(dpi), page);
        put_u32(body, 0);  // (no layers)
        put_u16(body, 0);  // (raw)
        const std::size_t row = (std::size_t(merged.width) + 7) / 8;
        for (std::uint32_t y = 0; y < merged.height; ++y) {
            std::string bits(row, '\0');
            for (std::uint32_t x = 0; x < merged.width; ++x) {
                if (merged.value(std::size_t(y) * merged.width + x, 0) < 0.5) bits[x / 8] = static_cast<char>(bits[x / 8] | (0x80 >> (x % 8)));
            }
            body += bits;
        }
    } else if (indexed) {
        const core::ColorTable& table = *page.rgb->table;
        put_u16(body, 1);
        put_u32(body, static_cast<std::uint32_t>(size.height));
        put_u32(body, static_cast<std::uint32_t>(size.width));
        put_u16(body, 8);
        put_u16(body, 2);
        std::string data(768, '\0');
        for (std::size_t i = 0; i < table.colors.size() && i < 256; ++i) {
            for (unsigned c = 0; c < 3; ++c) data[c * 256 + i] = static_cast<char>(table.colors[i][c]);
        }
        put_u32(body, 768);
        body += data;
        std::string count, transparent;
        put_u16(count, static_cast<std::uint32_t>(table.colors.size()));
        std::string blocks = resources(dpi).substr(4) + image_resource(1046, count) + render::psd_paths::resources(page);
        if (table.transparent) {
            put_u16(transparent, static_cast<std::uint32_t>(*table.transparent));
            blocks += image_resource(1047, transparent);
        }
        put_u32(body, u32_of(blocks.size()));
        body += blocks;
        put_u32(body, 0);  // (no layers)
        const std::string rgb = (made.image.mode() == "RGB" ? made.image : made.image.convert("RGB")).tobytes();
        std::string indices(std::size_t(size.width) * size.height, '\0');
        std::unordered_map<std::uint32_t, std::size_t> nearest;
        for (std::size_t i = 0; i < indices.size(); ++i) {
            const auto* p = reinterpret_cast<const unsigned char*>(rgb.data()) + i * 3;
            const std::uint32_t key = (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
            auto it = nearest.find(key);
            if (it == nearest.end()) it = nearest.emplace(key, table.nearest(p[0], p[1], p[2])).first;
            indices[i] = static_cast<char>(it->second);
        }
        std::string counts, packed;
        packbits_rows(Image::frombytes("L", size, indices), counts, packed);
        put_u16(body, 1);
        body += counts;
        body += packed;
    } else {
        core::PageColor color = *page.color;
        const int depth = color.depth == 32 ? 16 : color.depth;  // (Photoshop's Multichannel holds 8 or 16 bits)
        const core::InkPixels& merged = *made.inks;
        const unsigned n = merged.colours();
        put_u16(body, n + static_cast<unsigned>(page.channels.size()));
        put_u32(body, merged.height);
        put_u32(body, merged.width);
        put_u16(body, static_cast<std::uint32_t>(depth));
        put_u16(body, 7);
        put_u32(body, 0);  // (no colour mode data)
        // (the plates' names and looks as spots, then the page's channels')
        core::Page named = page;
        std::vector<core::Channel> plates;
        for (const core::Plate& plate : color.plates) {
            core::Channel ink;
            ink.name = plate.name;
            ink.kind = core::ChannelKind::Spot;
            ink.color = plate.color;
            ink.solidity = plate.solidity;
            plates.push_back(std::move(ink));
        }
        named.channels.insert(named.channels.begin(), plates.begin(), plates.end());
        body += with_channels(resources(dpi), named, 0);
        put_u32(body, 0);  // (no layers)
        const auto plane = [&](unsigned c) {
            std::string bytes;
            for (std::size_t i = 0; i < merged.pixel_count(); ++i) put_sample(bytes, 1.0 - merged.value(i, c), depth);
            return bytes;
        };
        if (depth == 8) {
            std::string counts, packed;
            for (unsigned c = 0; c < n; ++c) packbits_rows(Image::frombytes("L", size, plane(c)), counts, packed);
            put_channels_rle(page, size, counts, packed);
            put_u16(body, 1);
            body += counts;
            body += packed;
        } else {
            put_u16(body, 0);
            for (unsigned c = 0; c < n; ++c) body += plane(c);
            for (const core::Channel& channel : psd_order(page.channels)) body += channel_samples(page, channel, size, depth);
        }
    }
    return body;
}

// The page's PSD (export_page_psd) at `dpi`: its bytes. (`depth`: how deep in other contents it is, a smart object's
// contents written as the file of a placed layer — PC3 A3)
std::string page_psd_bytes(const core::Document& episode, const core::Page& page, std::int64_t dpi, const PsdText& text, PsdTextReport* report,
                           int depth) {
    if ((page.color && (page.color->bitmap || page.color->space == core::InkSpace::Multi)) || (!page.color && page.rgb && page.rgb->table)) {
        return page_psd_flat(episode, page, dpi);  // (color-model@1, PC1d: the merged picture only)
    }
    if (page.color) return page_psd_inks(episode, page, dpi, text, report, depth);
    if (page.rgb) return page_psd_rgb(episode, page, dpi, text, report, depth);
    const render::RenderOptions options = merged_options(page);
    const Image merged = render::render_page(page, static_cast<int>(dpi), options, &episode).image;
    const Image rgb = merged.mode() == "RGB" ? merged : merged.convert("RGB");
    Records records(rgb.size());
    records.masks_of(page);                    // (PC3 B2)
    records.patterns_of(episode);  // (patterns@1, PC4 E3)
    PlacedLayers placed(episode, dpi, depth);  // (PC3 A3)
    records.placed([&placed](const core::Layer& layer) { return placed.blocks(layer); });
    records.paper(page.spec.width_mm.value(), page.spec.height_mm.value());
    const auto each = [&](render::PageLayer&& layer) { records.add(std::move(layer)); };
    const std::function<void(const core::Layer&)> adjust_layers = adjustments(records, page, rgb.size(), episode);
    if (text.editable) {  // (PSD-01: each placed line's layers made here)
        PsdTextReport own;
        EditableLines lines(records, episode, static_cast<int>(dpi), rgb.size(), core::py_int(page.index.json()), report != nullptr ? *report : own);
        const std::function<void(const core::StoryLine&)> line_layers = [&](const core::StoryLine& line) { lines.add(line); };
        render::page_layers(page, episode, static_cast<int>(dpi), each, &line_layers, &adjust_layers, true);
    } else {
        render::page_layers(page, episode, static_cast<int>(dpi), each, nullptr, &adjust_layers, true);
    }
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    put_u16(body, 3 + static_cast<std::uint32_t>(page.channels.size()));
    put_u32(body, static_cast<std::uint32_t>(rgb.height()));
    put_u32(body, static_cast<std::uint32_t>(rgb.width()));
    put_u16(body, 8);
    put_u16(body, 3);
    put_u32(body, 0);  // (no colour mode data)
    body += with_channels(resources(dpi), page, 3);
    body += records.finish(placed.global() + render::pat::global_block(episode, core::patterns_on(page), 8));  // (PC4 E3: Patt)
    // the merged picture: RLE, every row's count of every channel first
    std::string counts, packed;
    for (const Image& channel : rgb.split()) packbits_rows(channel, counts, packed);
    put_channels_rle(page, rgb.size(), counts, packed);
    put_u16(body, 1);
    body += counts;
    body += packed;
    return body;
}

// The page's PSD (export_page_psd), put into `output` at `dest`.
void put_page_psd(detail::Output& output, const core::Document& episode, const core::Page& page, const fs::path& dest,
                  std::optional<std::int64_t> dpi_given, const PsdText& text = {}, PsdTextReport* report = nullptr) {
    const std::int64_t dpi = psd_dpi(episode, dpi_given);
    check_dpi(dpi);
    std::string body = page_psd_bytes(episode, page, dpi, text, report, 0);
    if (const auto xmp = book_xmp(episode)) body = psd_with_xmp(std::move(body), *xmp);  // (PC4 F-b: the book's File Info, 1060)
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    output.put(dest, body);
}

// A smart object's contents page as a PSD's bytes (export_contents_psd; the file of a placed layer, PC3 A3): its layers
// alone, without the paper and the panels' borders, the merged picture its layers over white; its own smart objects
// placed layers. A page in precise colour or inks as a page's PSD (with its paper).
std::string contents_psd_bytes(const core::Document& episode, const core::Page& page, std::int64_t dpi, int depth) {
    check_dpi(dpi);
    if (page.color || page.rgb) return page_psd_bytes(episode, page, dpi, PsdText{}, nullptr, depth);
    render::RenderOptions options = merged_options(page);
    options.contents = true;
    const Image drawn = render::render_contents(page, static_cast<int>(dpi), options, &episode).image;
    const Image rgb = render::alpha_composite(Image::create("RGBA", drawn.size(), render::Ink{255, 255, 255, 255}), drawn.convert("RGBA")).convert("RGB");
    Records records(rgb.size());
    records.masks_of(page);  // (PC3 B2)
    records.patterns_of(episode);  // (patterns@1, PC4 E3)
    PlacedLayers placed(episode, dpi, depth);
    records.placed([&placed](const core::Layer& layer) { return placed.blocks(layer); });
    records.paper(page.spec.width_mm.value(), page.spec.height_mm.value());
    const auto each = [&](render::PageLayer&& layer) {
        if (layer.name == "紙" || layer.name == "コマ枠" || layer.name == "ノンブル") return;  // (the contents' own: no paper, borders or nombre)
        records.add(std::move(layer));
    };
    const std::function<void(const core::Layer&)> adjust_layers = adjustments(records, page, rgb.size(), episode);
    render::page_layers(page, episode, static_cast<int>(dpi), each, nullptr, &adjust_layers, true);
    std::string body = "8BPS";
    put_u16(body, 1);
    body.append(6, '\0');
    put_u16(body, 3 + static_cast<std::uint32_t>(page.channels.size()));
    put_u32(body, static_cast<std::uint32_t>(rgb.height()));
    put_u32(body, static_cast<std::uint32_t>(rgb.width()));
    put_u16(body, 8);
    put_u16(body, 3);
    put_u32(body, 0);
    body += with_channels(resources(dpi), page, 3);
    body += records.finish(placed.global() + render::pat::global_block(episode, core::patterns_on(page), 8));  // (PC4 E3: Patt)
    std::string counts, packed;
    for (const Image& channel : rgb.split()) packbits_rows(channel, counts, packed);
    put_channels_rle(page, rgb.size(), counts, packed);
    put_u16(body, 1);
    body += counts;
    body += packed;
    return body;
}

}  // namespace

// (PSD-01) The strict editable export stops before anything is drawn when a line on the pages written uses what PSD
// text cannot keep.
void check_psd_text(const core::Document& episode, const PsdText& text, const core::Page* only, std::int64_t dpi) {
    if (!text.editable || text.rasterize) return;
    check_dpi(dpi);  // (before any line is set at it)
    std::vector<PsdTextIssue> issues = psd_text_issues(episode, dpi);
    if (only != nullptr) std::erase_if(issues, [&](const PsdTextIssue& issue) { return core::Num(issue.page) != only->index; });
    if (!issues.empty()) throw PsdTextRefused(std::move(issues));
}

fs::path export_page_psd(const core::Document& episode, const core::Page& page, const fs::path& dest, std::optional<std::int64_t> dpi,
                         const PsdText& text, PsdTextReport* report) {
    check_psd_text(episode, text, &page, psd_dpi(episode, dpi));
    detail::Output output;
    put_page_psd(output, episode, page, dest, dpi, text, report);
    output.commit();
    return dest;
}

std::vector<fs::path> export_psd_pages(const core::Document& episode, const fs::path& dest, std::optional<std::int64_t> dpi,
                                       const PsdText& text, PsdTextReport* report) {
    check_psd_text(episode, text, nullptr, psd_dpi(episode, dpi));
    detail::make_dirs(dest);
    detail::Output output;  // (one export: every page's PSD moved into place together)
    std::vector<fs::path> written;
    for (const auto& page : episode.pages) {
        written.push_back(detail::join(dest, stem(episode) + "_p" + detail::padded(page->index, 3) + ".psd"));
        put_page_psd(output, episode, *page, written.back(), dpi, text, report);
    }
    output.commit();
    return written;
}

fs::path export_psd(const core::Document& episode, const fs::path& dest, std::int64_t dpi, std::int64_t page, const PsdText& text,
                    PsdTextReport* report) {
    for (const auto& p : episode.pages) {
        if (p->index == core::Num(page)) return export_page_psd(episode, *p, dest, dpi, text, report);
    }
    throw core::Error("key", "no page " + std::to_string(page));  // (Python: StopIteration)
}

// smart-objects@1 (PC3 A1): a smart object's contents page as a PSD (コンテンツを書き出し…, リンクに変換…) — its layers
// alone, without the paper and the panels' borders (its panels only cut its layers), the merged picture its layers over
// white. A page in precise colour or inks is written as a page's PSD is (export_page_psd: with its paper).
fs::path export_contents_psd(const core::Document& episode, const core::Page& page, const fs::path& dest, std::optional<std::int64_t> dpi_given) {
    std::string body = contents_psd_bytes(episode, page, psd_dpi(episode, dpi_given), 0);
    detail::Output output;
    if (dest.has_parent_path()) detail::make_dirs(dest.parent_path());
    output.put(dest, body);
    output.commit();
    return dest;
}

std::vector<std::string> psd_smart_notes(const core::Document& episode) {
    bool filtered = false, linked = false;
    for (std::size_t i = 0; i < episode.pages.size(); ++i) {
        if (episode.is_deferred(i)) continue;
        for (const core::Layer& layer : episode.pages[i]->layers) {
            if (!core::smart::is_smart(layer) || !layer.visible || !layer.exportable) continue;
            filtered = filtered || core::smart::filters_drawn(layer);
            const core::SmartContents* contents = core::smart::contents_of(episode, layer);
            linked = linked || (contents != nullptr && contents->link.has_value());
        }
    }
    std::vector<std::string> notes;
    if (filtered) {
        notes.push_back("スマートフィルターは配置レイヤーの画素に焼き込んで書き出しました（Photoshop でコンテンツを編集すると、スマートフィルターは外れます）");
    }
    if (linked) notes.push_back("リンクしたスマートオブジェクトは、その写しを埋め込んだ配置レイヤーとして書き出しました");
    return notes;
}

}  // namespace genko::formats
