#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "render/codecs/format.hpp"
#include "render/codecs/image.hpp"
#include "render/codecs/options.hpp"

// What writing a picture in a format keeps and loses (PC4 F-0, SPEC.md FILE-01; PhotoCraft v0.2.0
// crates/codecs/src/fidelity.rs, MIT). The writer and the losses said before it writes come from one plan (the layout
// and sample it writes, from caps()), so a loss is said exactly when the file changes: the dialog of コピーを保存… and
// 書き出し形式… lists them, and the command line puts them in its notes.

namespace genko::render::codecs {

// What the writer stores: the layout and the sample.
struct Plan {
    Layout layout = Layout::Rgb;
    Sample sample = Sample::U8;
    bool operator==(const Plan&) const = default;
};

struct Loss {
    enum class Kind {
        WriteUnsupported,    // this build does not write the format (fatal)
        DepthReduced,        // fewer bits (16 → 8, a float → 16 bits or a half)
        RangeClipped,        // float samples outside 0..1 (HDR) cut by integer samples
        AlphaDiscarded,      // a picture that is not opaque written without its alpha
        AlphaBinarized,      // its alpha made all or nothing (GIF)
        CmykConverted,       // CMYK written as other colours (to: layout)
        ColorToGray,         // colours written as grey
        PaletteQuantized,    // 256 colours at most (GIF)
        LossyCompression,    // the writer loses what is not seen
        IccDropped,
        ExifDropped,
        XmpDropped,
        DpiDropped,
        TextDropped,
        DimensionsExceeded,  // larger than the format holds (fatal: max_width × max_height)
    };
    Kind kind = Kind::LossyCompression;
    Format format = Format::Png;
    Sample from = Sample::U8;  // (DepthReduced)
    Sample to = Sample::U8;
    Layout layout = Layout::Rgb;  // (CmykConverted: the colours it is written as)
    std::uint32_t max_width = 0;  // (DimensionsExceeded)
    std::uint32_t max_height = 0;

    // The write fails with it.
    bool fatal() const { return kind == Kind::WriteUnsupported || kind == Kind::DimensionsExceeded; }
    // In Japanese, as the notes say it (「16 bit を 8 bit にします」…).
    std::string text() const;
    bool operator==(const Loss&) const = default;
};

// The layout and sample `image` is written as in `format` (its caps' nearest: a sample the format holds without a change,
// else one that holds it — integers widen into floats without a loss —; the layout, its alpha left out where the format
// has none, CMYK and grey as RGB where the format holds neither).
Plan plan(const CodecImage& image, Format format, const EncodeOptions& options = {});
// What writing `image` as `format` loses, in this order: what stops the write, the depth, the range, the alpha, the
// colours, the compression, the metadata.
std::vector<Loss> losses(const CodecImage& image, Format format, const EncodeOptions& options = {});

}  // namespace genko::render::codecs
