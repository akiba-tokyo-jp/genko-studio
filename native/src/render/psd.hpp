#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/ink.hpp"
#include "core/json.hpp"
#include "render/image.hpp"
#include "render/psd_smart.hpp"
#include "render/psd_text.hpp"

// Photoshop files read as layers (Python's genko/psd.py: read_psd and its helpers), for import_psd. (ARCHITECTURE.md §1
// puts PSD/PSB reading in formats/; it sits in render next to the ABR reader, as import_psd, an op of render's, reads with
// it and it makes render::Image pictures. The PSD writer is formats/'s: formats/export.hpp.) PSD (version 1)
// and PSB (version 2); grey, bitmap, indexed (as grey), duotone, RGB, CMYK and the other modes as psd.py reads them;
// 1, 8, 16 and 32 bits; channels raw, PackBits, zip and zip with prediction. psd.py has its own reader (psd-tools is
// only a test dependency of the Python version, and Pillow's PsdImagePlugin, which reads the merged picture alone, is
// another reader again): this is that reader, decision for decision, with the same errors in the same order.
//
// Where psd.py builds every picture as it reads, this reads the file's structure and decodes every channel once to
// find the errors psd.py would raise (keeping none of the pixels), then makes each layer's picture when it is asked
// for: a file's pictures are never all held at once. Beyond psd.py, a hostile file is refused (ReadError, too_large)
// before it can take more memory than a page needs: a channel of more than kMaxChannelBytes or a layer of more than
// kMaxLayerPixels, and channels that decode to more than kMaxDecodedBytes together (counting a zip stream's whole
// output, which Python holds at once; what lies past the channel's size is not kept here). Python would go on until
// its memory ran out.

namespace genko::render::psd {

// What psd.read_psd raises, as Python raises it. `type` is the exception's name ("PSDError", "ValueError", "error" for
// struct.error and zlib.error, "KeyError", and Pillow's "OverflowError" and "MemoryError" for a picture of a side past
// C's int or of a width past its line size: a header of no area, 0 pixels high or wide); `caught` whether
// fileops.import_psd catches it (PSDError, struct.error, ValueError and IndexError: "the PSD cannot be read
// (<message>)"); the others go through it to apply_ops. `too_large`: this build's own refusal of a hostile file
// (caught as the others are).
class ReadError : public core::Error {
public:
    ReadError(std::string type, const std::string& message, bool caught, bool too_large = false);
    const std::string& type() const noexcept { return type_; }
    bool caught() const noexcept { return caught_; }
    bool too_large() const noexcept { return too_large_; }

private:
    std::string type_;
    bool caught_ = true;
    bool too_large_ = false;
};

struct Limits {
    std::int64_t max_channel_bytes;  // one channel decoded (before it is made 8-bit)
    std::int64_t max_layer_pixels;   // a layer's (or a mask's) own picture
    std::int64_t max_decoded_bytes;  // every channel decoded, together
};

// A channel at most 4 × limits::kPixels bytes (a 32-bit channel of the largest picture an op reads), a layer's picture
// at most limits::kPixels, and 8 GiB decoded in all.
Limits default_limits();

// psd.PSDLayer without its pictures (File::image and File::mask make them).
struct Layer {
    std::string name;  // UTF-8
    bool folder = false;
    double opacity = 1.0;
    bool visible = true;
    std::string blend = "normal";
    bool clip = false;
    std::optional<std::size_t> parent;  // the index (in layers()) of the folder it is in
    std::string kind = "pixels";        // pixels | folder | text | adjust | smart
    bool has_mask = false;
    std::size_t record = 0;  // (pixel layers) the layer record it was read from
    // (PC2, the user's decision 5) an adjustment layer a codec read (render/psd_adjust.hpp): the correction's adjust.
    // One no codec reads is left out (skipped), as psd.py leaves every one out.
    std::optional<core::Json> adjust;
    // (PC2a) the lookup tables its adjust uses (a color lookup's: the table its block holds, as GKLU bytes by their
    // asset ref), which import_psd takes into the book with the correction.
    std::map<std::string, core::Bytes> luts;
    // (PC3 A3, the user's decision 3) a placed layer (kind smart): its block read (render/psd_smart.hpp) — SoLd, else SoLE,
    // else PlLd, with PlLd's corners where SoLd has none. Its pixels are Photoshop's picture of it (image()).
    std::optional<Placed> placed;
};

class File {
public:
    // psd.read_psd(data): ReadError with read_psd's errors. `lab`: a Lab file is read too (color-mode@1: into a CMYK or
    // Lab page; read_psd refuses one).
    static File read(std::string data, const Limits& limits = default_limits(), bool lab = false);

    // (width, height) of the canvas, as the header has them (32 bits each).
    std::int64_t width() const { return width_; }
    std::int64_t height() const { return height_; }
    double dpi() const { return dpi_; }
    // psd.MODE_NAMES (the number as text for another mode).
    const std::string& mode_name() const { return mode_name_; }
    // Bottom to top, folders after the layers in them, as read_psd lists them.
    const std::vector<Layer>& layers() const { return layers_; }
    const std::vector<std::string>& skipped() const { return skipped_; }
    // (PC2) the names psd.py skips: skipped() and, in their places, the adjustment layers a codec read (their names as
    // the records have them).
    const std::vector<std::string>& python_skipped() const { return python_skipped_; }

    // A pixel layer's picture over the whole canvas (PSDLayer.image: RGBA).
    Image image(const Layer& layer) const;
    // Its mask over the whole canvas (PSDLayer.mask: "L", white shows); nothing when it has none.
    std::optional<Image> mask(const Layer& layer) const;
    // The merged picture (PSDFile.merged: RGBA), nothing where read_psd has none.
    std::optional<Image> merged() const;
    // color-mode@1: a pixel layer's picture over the whole canvas, and the merged picture, as a CMYK or Lab page's inks
    // (CMYK from the file's own profile or the page's, Lab from Lab, the rest from RGB; 8-bit samples as read).
    core::InkPixels image_inks(const Layer& layer, const core::PageColor& color) const;
    // (`own_profile` false: a CMYK file's inks taken as the page's, its profile left aside — PC3 A3, a smart object's
    // file kept as its numbers)
    std::optional<core::InkPixels> merged_inks(const core::PageColor& color, bool own_profile = true) const;
    // The ICC profile the file carries (image resource 1039), or "".
    const std::string& icc() const { return icc_; }

    // PC3 A3 (smart-objects@1: a PSD or PSB as a smart object's file, and its placed layers). The header's depth (1, 8,
    // 16, 32), colour mode number and channels; whether it is a PSB.
    int depth() const { return depth_; }
    int mode() const { return mode_; }
    int channel_count() const { return nchan_; }
    bool psb() const { return psb_; }
    // Whether its merged picture is Photoshop's picture of the layers (image resource 1057's hasRealMergedData; true when
    // the file does not say): a file saved without maximised compatibility holds a blank one.
    bool real_merged() const { return real_merged_; }
    // The merged picture in the file's own colours, 8 bits a sample: "L" (grey, bitmap, duotone, multichannel, indexed as
    // psd.py reads them), "LA", "RGB", "RGBA", "CMYK" (its inks), "LAB" (its L, a+128, b+128 bytes); the alpha a flat
    // file's transparency. Nothing where it has none (merged()).
    std::optional<Image> merged_picture() const;
    // The merged picture with every bit: per pixel its colour channels and (a flat file's) its alpha as floats — 0..1 for
    // 1, 8 and 16 bits (a CMYK file's as ink, 1 full), as stored for 32 (linear light, HDR kept); rows from the top.
    struct Deep {
        std::int64_t width = 0, height = 0;
        unsigned colours = 3;
        bool alpha = false;
        std::vector<float> samples;
    };
    std::optional<Deep> merged_deep() const;
    // Its visible pixel layers (in visible folders) laid over each other in normal at their opacity, through their masks
    // ("RGBA" over the canvas): the picture of a file with no merged picture, or a blank one (real_merged()).
    Image composited() const;
    // The file its linked-layer blocks (the global lnk2, lnk3, lnkD) hold for a placed layer's uuid; none when none
    // does.
    std::optional<LinkedItem> linked(std::string_view uuid) const;
    // A placed layer's block as the layer record holds it (its payload: what import_psd keeps to write it back); empty
    // for another layer.
    std::string_view placed_block(const Layer& layer) const;
    // channels@1 (this build's own, not psd.py's): the merged picture's extra channels — the alpha and spot channels past
    // its colours (and past a flat file's transparency) —, each with its name (image resource 1045, else 1006) and how
    // it shows (1077: 0 selected areas, 1 masked areas, 2 spot; its RGB colour; its opacity, a spot's solidity, in
    // percent) and its 8-bit samples over the canvas as stored (the grey Photoshop shows: a spot's and a selected-areas
    // channel's 0 is its colour, a masked-areas channel's 255 is selected). None when the file has no merged picture this
    // reads. masks@1 (PC3 B1): the one image resource 1022 names is the Quick Mask the file was saved in.
    struct ExtraChannel {
        std::string name;
        int kind = 1;
        std::array<int, 3> color{255, 0, 0};
        int opacity = 50;
        std::string samples;
        bool quick_mask = false;
    };
    std::vector<ExtraChannel> extra_channels() const;
    // PSD-01 (not psd.py's: the structure checks of the editable PSD): a layer's text, read from its TySh block
    // (render/psd_text.hpp), and the stroke of its layer effects (its lfx2 block); nothing for a layer without one.
    // core::Error("format") for a block that cannot be read.
    std::optional<TextLayer> text(const Layer& layer) const;
    std::optional<Stroke> stroke(const Layer& layer) const;
    // vector@1 (PC3 C-a, render/psd_paths): a layer's vmsk (or vsms) block's payload — its vector mask —, nothing for a layer
    // without one; and the file's path resources (its saved paths 2000–2997 with their names, its work path 1025, its
    // clipping path 2999), in the file's order.
    std::optional<std::string_view> vector_mask(const Layer& layer) const;
    struct PathResource {
        unsigned id = 0;
        std::string name;  // (the resource's Pascal name, as its bytes)
        std::string data;
    };
    const std::vector<PathResource>& path_resources() const { return path_resources_; }

    struct ChannelRef {
        int id = 0;
        std::uint64_t start = 0;  // where its compression is
        std::uint64_t end = 0;
        std::int64_t width = 0;
        std::int64_t height = 0;
        std::uint64_t length = 0;  // its 8-bit samples' count
    };
    struct Record {
        std::int64_t left = 0, top = 0, right = 0, bottom = 0;
        std::vector<ChannelRef> data;  // the channels read, the last of each id (Python's dict)
        int opacity = 255;
        bool clip = false;
        bool visible = true;
        std::string blend = "normal";
        std::string name;
        std::uint32_t section = 0;
        std::string kind = "pixels";
        bool mask = false;
        std::int64_t mask_left = 0, mask_top = 0, mask_right = 0, mask_bottom = 0;
        int mask_default = 0;
        bool mask_disabled = false;
        // (PSD-01) where its TySh and lfx2 blocks' payloads are in the file (size 0: none)
        std::uint64_t tysh_at = 0, tysh_size = 0;
        std::uint64_t lfx2_at = 0, lfx2_size = 0;
        // (PC2) its adjustment block's key and payload (render/psd_adjust.hpp; the key empty: none, or a fill layer's)
        std::string adjust_key;
        std::uint64_t adjust_at = 0, adjust_size = 0;
        std::uint64_t cged_at = 0, cged_size = 0;  // (its CgEd block: brightness_contrast's modern settings)
        // (PC3 A3) its placed layer blocks' payloads: SoLd, SoLE, PlLd (size 0: none)
        std::uint64_t sold_at = 0, sold_size = 0, sole_at = 0, sole_size = 0, plld_at = 0, plld_size = 0;
        std::uint64_t vmsk_at = 0, vmsk_size = 0;  // (vector@1, PC3 C-a: its vmsk or vsms block — its vector mask)
        const ChannelRef* channel(int id) const;
    };

private:
    std::string data_;
    bool psb_ = false;
    std::int64_t width_ = 0;
    std::int64_t height_ = 0;
    int nchan_ = 0;
    int depth_ = 0;
    int mode_ = 0;
    double dpi_ = 72.0;
    std::string mode_name_;
    std::string icc_;
    std::string channel_names_;    // (1045, else 1006)
    bool unicode_names_ = false;
    std::string display_info_;     // (1077)
    bool real_merged_ = true;      // (1057)
    std::vector<std::pair<std::uint64_t, std::uint64_t>> linked_blocks_;  // (PC3 A3: the global lnk2, lnk3, lnkD: where, how long)
    std::optional<unsigned> quick_mask_;  // (1022: the Quick Mask's channel)
    std::vector<PathResource> path_resources_;  // (vector@1: 2000–2997, 1025, 2999)
    std::uint64_t merged_at_ = 0;
    Limits limits_{};
    std::vector<Record> records_;
    std::vector<Layer> layers_;
    std::vector<std::string> skipped_;
    std::vector<std::string> python_skipped_;

    // What read_psd makes of the merged picture, worked out without its pixels: whether it has one (`exists`) and
    // whether it gets as far as making a picture (`reaches`: a 1-bit plane is made a picture as it is read).
    struct MergedPlan {
        bool exists = false;
        bool reaches = false;
        unsigned compression = 0;
        std::uint64_t counts_at = 0;
        std::vector<std::uint64_t> starts;  // (the planes used)
    };
    MergedPlan plan_merged() const;
    std::string samples(const ChannelRef& channel) const;
    Image part(const Record& record) const;
    std::map<int, std::string> part_channels(const Record& record) const;
    std::optional<std::map<int, std::string>> merged_channels() const;
    // (each merged plane's raw bytes at the file's depth: the colours', and a flat file's transparency)
    std::optional<std::map<int, std::string>> merged_raw() const;
};

// psd.BLEND_KEYS: the blend mode of a layer record's key ("normal" for one not there).
std::string blend_of(std::string_view key);

}  // namespace genko::render::psd
