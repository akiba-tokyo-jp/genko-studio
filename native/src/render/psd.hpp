#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "render/image.hpp"

// Photoshop files read as layers (Python's genko/psd.py: read_psd and its helpers), for import_psd. (ARCHITECTURE.md §1
// puts PSD/PSB reading in formats/, which has no library yet: it sits in render next to the ABR reader, as it makes
// render::Image pictures.) PSD (version 1)
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
    std::string kind = "pixels";        // pixels | folder | text | adjust
    bool has_mask = false;
    std::size_t record = 0;  // (pixel layers) the layer record it was read from
};

class File {
public:
    // psd.read_psd(data): ReadError with read_psd's errors.
    static File read(std::string data, const Limits& limits = default_limits());

    // (width, height) of the canvas, as the header has them (32 bits each).
    std::int64_t width() const { return width_; }
    std::int64_t height() const { return height_; }
    double dpi() const { return dpi_; }
    // psd.MODE_NAMES (the number as text for another mode).
    const std::string& mode_name() const { return mode_name_; }
    // Bottom to top, folders after the layers in them, as read_psd lists them.
    const std::vector<Layer>& layers() const { return layers_; }
    const std::vector<std::string>& skipped() const { return skipped_; }

    // A pixel layer's picture over the whole canvas (PSDLayer.image: RGBA).
    Image image(const Layer& layer) const;
    // Its mask over the whole canvas (PSDLayer.mask: "L", white shows); nothing when it has none.
    std::optional<Image> mask(const Layer& layer) const;
    // The merged picture (PSDFile.merged: RGBA), nothing where read_psd has none.
    std::optional<Image> merged() const;

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
    std::uint64_t merged_at_ = 0;
    Limits limits_{};
    std::vector<Record> records_;
    std::vector<Layer> layers_;
    std::vector<std::string> skipped_;

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
};

// psd.BLEND_KEYS: the blend mode of a layer record's key ("normal" for one not there).
std::string blend_of(std::string_view key);

}  // namespace genko::render::psd
