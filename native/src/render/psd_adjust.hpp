#pragma once

#include <functional>
#include <map>
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
// what its group filled. A block may be written from more than one kind (hue2: hue_saturation's, and Python's hue as
// the nearest, design 4.4); it is read as the first of them (hue2: hue_saturation).
const std::vector<AdjustCodec>& adjust_codecs();
// (by key: the read and write filled — into the rows of that key, or, when the hook names its kind, that kind's row)
using CodecHooks = std::vector<std::pair<std::string_view, AdjustCodec>>;
CodecHooks tone_codecs();   // PC2a: render/psd_adjust.cpp (levl, curv, brit, post, thrs, nvrt, expA, grdm, clrL)
CodecHooks color_codecs();  // PC2b: render/psd_adjust_color.cpp (hue2 — and Python's hue —, vibA, blnc, blwh, phfl, mixr, selc)

// The notes' words (the person's: plain Japanese). An adjustment block's name (its command's, Photoshop's: レベル補正,
// 色相・彩度 …, the user's decision 8; a block this build does not know: its key), a correction kind's (the name of the
// block that holds it; another kind: its key), and the notes of a block not read ("PSD の調整レイヤー（levl: レベル補正）は
// <why>ため、読み込みませんでした") and of a correction layer not written ("レベル補正の補正レイヤーは<why>ため、PSD の調整
// レイヤーにせず統合画像にだけ入れました").
std::string adjust_label(std::string_view key);
std::string kind_label(std::string_view kind);
void note_unread(std::vector<std::string>& notes, std::string_view key, std::string_view why);
void note_unwritten(std::vector<std::string>& notes, std::string_view kind, std::string_view why);

// What a codec may ask of the file it reads or writes (the two ways set it while they call it).
struct AdjustContext {
    // The PSD's colour mode (its levels' and curves' channel records: rgb red, green, blue; gray its one; cmyk cyan …
    // black; lab lightness, a, b; other none).
    std::string_view mode = "rgb";
    std::string_view companion;            // (reading) the layer's CgEd block's payload (brightness_contrast's)
    // (reading) where a block's lookup table goes (clrL's: the table its file text holds, by its asset ref — import_psd
    // takes it into the book with its correction); none: such a block is not read
    std::map<std::string, core::Bytes>* tables = nullptr;
    const core::Document* doc = nullptr;  // (writing) the book: a lookup table's asset (only the book's own)
};
const AdjustContext& adjust_context();
// The PSD's mode by its number in the file header (rgb, gray, cmyk, lab, other).
std::string_view adjust_mode(int psd_mode);

// While one lives (on any thread), no codec reads or writes: every adjustment layer is left out (skipped) as psd.py
// leaves them, and a correction layer is in the merged picture only, as psd.py writes it — the view of the Python
// reference, which the contract tests compare with (the user's decision 5: this build reads and writes them).
class PythonView {
public:
    PythonView();
    ~PythonView();
    PythonView(const PythonView&) = delete;
    PythonView& operator=(const PythonView&) = delete;
};
// Is one alive? (PC4 F-d: then import_psd keeps nothing of psd-keep@1, reads no lock, and reads the layers in 8 bits, and
// the reader reads no layers from a global Lr16 or Lr32 block — the Python reference's view too)
bool python_view();

// An adjustment block read into a correction's adjust; none when no codec reads it (`notes` says why).
std::optional<core::Json> read_adjustment(std::string_view key, std::string_view payload, std::vector<std::string>& notes,
                                          const AdjustContext& context = {});

// A correction's adjust as an adjustment block; none when no codec writes its kind (`notes` says why). A companion
// block written after it (brightness_contrast's CgEd: its modern settings), when it has one.
struct AdjustBlock {
    std::string key;
    std::string payload;
    std::string companion_key;
    std::string companion;
};
std::optional<AdjustBlock> write_adjustment(const core::Json& adjust, const core::Page& page, std::vector<std::string>& notes,
                                            const core::Document* doc = nullptr);

// An adjustment layer read (its adjust, its blend in the PSD) as the correction import_psd puts on `page`: its blend its
// native.blend, read as an op reads it — what the page cannot hold of it (a blend a Lab or multichannel page does not
// composite, channel records on a multichannel page) left out.
core::Json imported_adjust(const core::Json& adjust, std::string_view blend, const core::Page& page);

}  // namespace genko::render::psd
