#pragma once

// Small PSD and PSB files made for the tests (PC3 A3): a header, image resources, layer records with their channels raw and
// their tagged blocks, global tagged blocks, and the merged picture raw — as Photoshop lays them out (Adobe's file format,
// upstream PhotoCraft's tests make theirs the same way, smart_cmds/tests.rs:359-397). Not a writer of this build's: what
// the tests read with render/psd and import_psd, and psd-tools reads as Photoshop's.

#include <cstdint>
#include <string>
#include <vector>

namespace genko::test::psd_build {

inline void u8(std::string& out, unsigned v) { out += static_cast<char>(v & 0xff); }
inline void u16(std::string& out, std::uint32_t v) {
    u8(out, v >> 8);
    u8(out, v);
}
inline void u32(std::string& out, std::uint32_t v) {
    u16(out, v >> 16);
    u16(out, v & 0xffff);
}
inline void u64(std::string& out, std::uint64_t v) {
    u32(out, static_cast<std::uint32_t>(v >> 32));
    u32(out, static_cast<std::uint32_t>(v & 0xffffffffULL));
}

// A tagged block: signature, key, length (8 bytes in a PSB for `long_length`), its payload padded to `pad` bytes.
inline std::string block(const std::string& key, std::string payload, bool long_length = false, std::size_t pad = 4) {
    while (payload.size() % pad != 0) payload += '\0';
    std::string out = "8BIM" + key;
    if (long_length) u64(out, payload.size());
    else u32(out, static_cast<std::uint32_t>(payload.size()));
    return out + payload;
}

struct Layer {
    std::string name;
    int left = 0, top = 0, right = 0, bottom = 0;
    // each channel's id and its samples, raw at the file's depth (rows of right - left samples)
    std::vector<std::pair<int, std::string>> channels;
    std::string blocks;  // its tagged blocks after its name
    int opacity = 255;
    bool hidden = false;
};

struct File {
    int version = 1;  // 2: PSB
    int width = 0, height = 0;
    int depth = 8;
    int mode = 3;     // 1 grey, 3 RGB, 4 CMYK, 9 Lab
    double dpi = 72;
    bool real_merged = true;  // (image resource 1057's hasRealMergedData; written only when false)
    std::vector<Layer> layers;  // bottom to top
    std::string global;         // the global tagged blocks (after the global layer mask)
    std::vector<std::string> merged;  // the merged picture's planes, raw at the file's depth (none: no merged picture's data)
};

inline std::string bytes(const File& f) {
    const bool psb = f.version == 2;
    std::string out = "8BPS";
    u16(out, static_cast<std::uint32_t>(f.version));
    out.append(6, '\0');
    u16(out, static_cast<std::uint32_t>(f.merged.empty() ? (f.mode == 4 ? 4 : f.mode == 1 ? 1 : 3) : f.merged.size()));
    u32(out, static_cast<std::uint32_t>(f.height));
    u32(out, static_cast<std::uint32_t>(f.width));
    u16(out, static_cast<std::uint32_t>(f.depth));
    u16(out, static_cast<std::uint32_t>(f.mode));
    u32(out, 0);  // (no colour mode data)
    std::string resources;
    {
        std::string res;
        const auto fixed = static_cast<std::uint32_t>(f.dpi * 65536);
        u32(res, fixed);
        u16(res, 1);
        u16(res, 1);
        u32(res, fixed);
        u16(res, 1);
        u16(res, 1);
        resources += "8BIM";
        u16(resources, 1005);
        u16(resources, 0);
        u32(resources, static_cast<std::uint32_t>(res.size()));
        resources += res;
        if (!f.real_merged) {
            std::string info;
            u32(info, 1);
            u8(info, 0);  // (hasRealMergedData: no)
            u32(info, 0);
            u32(info, 0);
            u32(info, 1);
            resources += "8BIM";
            u16(resources, 1057);
            u16(resources, 0);
            u32(resources, static_cast<std::uint32_t>(info.size()));
            resources += info;
            if (info.size() % 2) resources += '\0';
        }
    }
    u32(out, static_cast<std::uint32_t>(resources.size()));
    out += resources;
    // the layers and masks
    std::string info;
    if (!f.layers.empty()) {
        std::string records, data;
        u16(records, static_cast<std::uint32_t>(f.layers.size()));
        for (const Layer& l : f.layers) {
            u32(records, static_cast<std::uint32_t>(l.top));
            u32(records, static_cast<std::uint32_t>(l.left));
            u32(records, static_cast<std::uint32_t>(l.bottom));
            u32(records, static_cast<std::uint32_t>(l.right));
            u16(records, static_cast<std::uint32_t>(l.channels.size()));
            for (const auto& [id, samples] : l.channels) {
                u16(records, static_cast<std::uint32_t>(static_cast<std::int16_t>(id)) & 0xffff);
                if (psb) u64(records, samples.size() + 2);
                else u32(records, static_cast<std::uint32_t>(samples.size() + 2));
                u16(data, 0);  // (raw)
                data += samples;
            }
            records += "8BIMnorm";
            u8(records, static_cast<unsigned>(l.opacity));
            u8(records, 0);
            u8(records, l.hidden ? 2 : 0);
            u8(records, 0);
            std::string extra;
            u32(extra, 0);  // (no mask)
            u32(extra, 0);  // (no blending ranges)
            std::string name(1, static_cast<char>(l.name.size()));
            name += l.name;
            while (name.size() % 4 != 0) name += '\0';
            extra += name + l.blocks;
            u32(records, static_cast<std::uint32_t>(extra.size()));
            records += extra;
        }
        std::string layer_info = records + data;
        if (layer_info.size() % 2) layer_info += '\0';
        if (psb) u64(info, layer_info.size());
        else u32(info, static_cast<std::uint32_t>(layer_info.size()));
        info += layer_info;
    } else {
        if (psb) u64(info, 0);
        else u32(info, 0);
    }
    u32(info, 0);  // (no global layer mask)
    info += f.global;
    if (psb) u64(out, info.size());
    else u32(out, static_cast<std::uint32_t>(info.size()));
    out += info;
    // the merged picture, raw
    u16(out, 0);
    for (const std::string& plane : f.merged) out += plane;
    return out;
}

// Samples of one value, `count` of them, at `depth` (16: big-endian; 32: the float).
inline std::string samples(std::size_t count, std::uint32_t value, int depth) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) {
        if (depth == 32) u32(out, value);
        else if (depth == 16) u16(out, value);
        else u8(out, value);
    }
    return out;
}

}  // namespace genko::test::psd_build
