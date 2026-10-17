#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// PC2 (補正・色管理, the user's decision 5: a PSD's adjustment layers are always read and written): Photoshop's
// adjustment layers and Genko's correction layers, one kind at a time — the layer record's adjustment block (its key:
// levl, curv, brit, …; PhotoCraft's io/adjust_map.rs) and the correction's "adjust" (core/adjust_kinds.hpp). Both ways
// are here (render: import_psd reads with them, render/import_psd.cpp; the PSD writer writes with them,
// formats/psd_adjust.hpp). Each kind's codec is its group's (PC2a: this file's tone_codecs; PC2b:
// render/psd_adjust_color.cpp's color_codecs); a kind with none is read as before — the adjustment layer left out, its
// name in the notes — and written as before (its change in the merged picture only: Python's PSD).

namespace genko::render::psd {

struct AdjustCodec {
    std::string_view key;    // the layer record's tagged block
    std::string_view kind;   // the correction's kind
    std::string_view group;  // who writes its codec
    // The block's payload → the correction's adjust (its normal form; none: not read — `notes` says why).
    std::function<std::optional<core::Json>(std::string_view payload, std::vector<std::string>& notes)> read;
    // The correction's adjust → the block's payload (none: not written — `notes` says why). Python's form, which a PSD
    // adjustment cannot always hold (Python's hue, bitonal, the gradient map's Pillow lightness), by the nearest one and
    // a note (design 4.4).
    std::function<std::optional<std::string>(const core::Json& adjust, const core::Page& page, std::vector<std::string>& notes)> write;
};

// Every adjustment block this build knows (render/psd.cpp's adjust_tag but the fill layers SoCo, GdFl, PtFl), each with
// what its group filled.
const std::vector<AdjustCodec>& adjust_codecs();
using CodecHooks = std::vector<std::pair<std::string_view, AdjustCodec>>;  // (by key: the read and write filled)
CodecHooks tone_codecs();   // PC2a: render/psd_adjust.cpp (levl, curv, brit, post, thrs, nvrt, expA, grdm, clrL)
CodecHooks color_codecs();  // PC2b: render/psd_adjust_color.cpp (hue2, vibA, blnc, blwh, phfl, mixr, selc)

// An adjustment block read into a correction's adjust; none when no codec reads it (`notes` says why).
std::optional<core::Json> read_adjustment(std::string_view key, std::string_view payload, std::vector<std::string>& notes);

// A correction's adjust as an adjustment block; none when no codec writes its kind (`notes` says why).
struct AdjustBlock {
    std::string key;
    std::string payload;
};
std::optional<AdjustBlock> write_adjustment(const core::Json& adjust, const core::Page& page, std::vector<std::string>& notes);

}  // namespace genko::render::psd
