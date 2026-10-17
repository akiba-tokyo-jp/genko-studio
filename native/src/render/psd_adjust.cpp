// The codecs of a PSD's adjustment layers (render/psd_adjust.hpp): the table of every adjustment block and the two
// ways through it. PC2a writes the tone kinds' codecs here (tone_codecs: PhotoCraft's io/adjust_map.rs, attribution in
// native/third_party/photocraft/NOTICE.md).

#include "render/psd_adjust.hpp"

namespace genko::render::psd {

namespace {

AdjustCodec codec(std::string_view key, std::string_view kind, std::string_view group) {
    AdjustCodec out;
    out.key = key;
    out.kind = kind;
    out.group = group;
    return out;
}

std::vector<AdjustCodec> make_table() {
    std::vector<AdjustCodec> all{codec("levl", "levels", "PC2a"),         codec("curv", "curve", "PC2a"),
                                 codec("brit", "brightness_contrast", "PC2a"), codec("post", "posterize", "PC2a"),
                                 codec("thrs", "threshold", "PC2a"),       codec("nvrt", "invert", "PC2a"),
                                 codec("expA", "exposure", "PC2a"),        codec("grdm", "gradient_map", "PC2a"),
                                 codec("clrL", "color_lookup", "PC2a"),    codec("hue2", "hue_saturation", "PC2b"),
                                 codec("vibA", "vibrance", "PC2b"),        codec("blnc", "color_balance", "PC2b"),
                                 codec("blwh", "black_white", "PC2b"),     codec("phfl", "photo_filter", "PC2b"),
                                 codec("mixr", "channel_mixer", "PC2b"),   codec("selc", "selective_color", "PC2b")};
    for (const CodecHooks& hooks : {tone_codecs(), color_codecs()}) {
        for (const auto& [key, given] : hooks) {
            for (AdjustCodec& each : all) {
                if (each.key != key) continue;
                if (given.read) each.read = given.read;
                if (given.write) each.write = given.write;
            }
        }
    }
    return all;
}

}  // namespace

// --- PC2a: the tone kinds (levl, curv, brit, post, thrs, nvrt, expA, grdm) and clrL -----------------------------------

CodecHooks tone_codecs() { return {}; }

// --- the table and the two ways ---------------------------------------------------------------------------------------

const std::vector<AdjustCodec>& adjust_codecs() {
    static const std::vector<AdjustCodec> table = make_table();
    return table;
}

std::optional<core::Json> read_adjustment(std::string_view key, std::string_view payload, std::vector<std::string>& notes) {
    for (const AdjustCodec& each : adjust_codecs()) {
        if (each.key != key) continue;
        if (!each.read) {
            notes.push_back("the adjustment layer " + std::string(key) + " (" + std::string(each.kind) + ") is not read by this build yet");
            return std::nullopt;
        }
        return each.read(payload, notes);
    }
    notes.push_back("the adjustment layer " + std::string(key) + " is not one this build reads");
    return std::nullopt;
}

std::optional<AdjustBlock> write_adjustment(const core::Json& adjust, const core::Page& page, std::vector<std::string>& notes) {
    const auto kind = adjust.is_object() ? adjust.find("kind") : adjust.end();
    if (!adjust.is_object() || kind == adjust.end() || !kind->is_string()) return std::nullopt;
    for (const AdjustCodec& each : adjust_codecs()) {
        if (each.kind != kind->get<std::string>()) continue;
        if (!each.write) return std::nullopt;  // (not written yet: its change is in the merged picture, as Python's PSD)
        std::optional<std::string> payload = each.write(adjust, page, notes);
        if (!payload) return std::nullopt;
        return AdjustBlock{std::string(each.key), std::move(*payload)};
    }
    return std::nullopt;
}

}  // namespace genko::render::psd
