#pragma once

// How a picture is written (PC4 F-0, F-a; PhotoCraft's EncodeOptions, crates/codecs/src/options.rs). jpeg_quality is also
// a lossy WebP's and an AVIF's quality; webp_lossless false writes a lossy WebP (PhotoCraft's writes lossless only).

namespace genko::render::codecs {

enum class PngCompression { None, Fast, Default, Best };
enum class TiffCompression { None, Lzw, Deflate, PackBits };
// OpenEXR's lossless compressions only.
enum class ExrCompression { None, Rle, Zip1, Zip16, Piz };
// (PC4 F-a review) Netpbm's kind, as an extension names it: .pbm P4 (white and black), .pgm P5, .ppm P6, .pam P7, .pfm PF or
// Pf; Any (.pnm), the picture's own (floats PFM, grey P5, RGB P6, any other P7).
enum class PnmKind { Any, Bitmap, Graymap, Pixmap, Arbitrary, Float };

struct EncodeOptions {
    int jpeg_quality = 90;  // 1..100 (AVIF's quality too)
    bool jpeg_chroma_subsampling = true;  // 4:2:0 (false: 4:4:4)
    PngCompression png_compression = PngCompression::Default;
    bool png_interlaced = false;
    bool webp_lossless = true;
    TiffCompression tiff_compression = TiffCompression::Deflate;
    ExrCompression exr_compression = ExrCompression::Zip16;
    PnmKind pnm_kind = PnmKind::Any;
    bool embed_icc = true;       // the ICC profile, where the format holds one
    bool embed_metadata = true;  // EXIF, XMP, resolution and text, where the format holds them
};

}  // namespace genko::render::codecs
