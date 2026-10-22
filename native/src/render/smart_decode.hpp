#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"
#include "render/image.hpp"

// smart-objects@1 (PC3 A0, decision 2: 画像を読み込む… places a smart object of the file itself): the readers of a smart
// object's file, by its format (kReaders: PNG, JPEG, TIFF, WebP, BMP, GIF, PSD/PSB — a later step adds a format with a
// row). The file's bytes are never changed: the 8-bit page draws them as open_image reads any picture (a placed picture's
// pixels); a page in precise colour draws a PNG of 16 bits and a TIFF of 16 or 32 bits with all their bits (deep_picture);
// a CMYK, grey or Lab page takes a CMYK, grey or Lab file into its inks as they are (file_inks). Its colours come onto the
// page by the use the contents keep (core::SmartContents::profile): keep (its numbers), embedded (from the profile it
// carries), assign (from the profile given).

namespace genko::render::smart {

// The most bytes a smart object's file holds (the asset store's limit, storage::kAssetMaxBytes).
inline constexpr std::uint64_t kFileMaxBytes = 256ull * 1024 * 1024;

// What a file is, as placing it records it (core::SmartContents): its format and suffix, its size, the bits of a sample, its
// colours (gray, rgb, cmyk, lab; with an alpha: graya, rgba, …), the resolution it says (0: none) and the profile it carries.
struct FileInfo {
    std::string format;
    std::string suffix;
    std::int64_t width = 0;
    std::int64_t height = 0;
    int depth = 8;
    std::string mode = "rgb";
    double dpi = 0;
    std::optional<std::string> icc;
};

// The file looked at whole before it is placed: its header read, and its pixels decoded as the drawing decodes them (a file
// that cannot be drawn never becomes contents). core::Error: "too_large" for more than kFileMaxBytes, "unidentified_image"
// for a kind of file this build does not read (or a suffix that is not its kind), "image_too_large" past the drawing's
// limits (a decompression bomb), "format" for one cut short or broken; NotYetPorted for a kind not read yet.
FileInfo look_at(std::string_view bytes, std::string_view suffix);

// The file as the 8-bit page reads it: open_image's picture (a placed picture's), or — a PNG or TIFF of 16 or 32 bits that
// open_image does not read — its samples rounded to 8 bits; a PSD or PSB (PC3 A3) read by render/psd: its merged picture
// in its own colours (Lab as sRGB; an indexed file's through its palette), or its visible layers laid over each other
// where it has none or a blank one. core::Error as look_at's.
Image eight_bit_picture(std::string_view bytes);

// A picture with every bit kept: per pixel `channels` floats (0..1 for 8 and 16 bits, as stored for 32: HDR kept), the last
// the alpha when it has one (straight), rows from the top. `mode` as FileInfo's (gray, rgb, cmyk with or without "a").
struct DeepPicture {
    std::int64_t width = 0;
    std::int64_t height = 0;
    unsigned channels = 3;
    bool alpha = false;
    std::string mode = "rgb";
    int depth = 8;
    std::vector<float> samples;
};

// PC4 F-0 (render/codecs): what look_at reads of a file's header without its pixels — the bits a sample of a PNG or a
// TIFF holds (8 for any other file, and for one whose header cannot be read), the samples a pixel of it holds as
// deep_picture reads it (a PNG's after its palette and tRNS are expanded, a TIFF's own; 4 for any other file and for one
// whose header cannot be read) and the resolution the file says (0: none).
int stored_depth(std::string_view bytes);
int stored_samples(std::string_view bytes);
double stored_dpi(std::string_view bytes);

// The file's pixels with all their bits: a PNG of 16 bits, a TIFF of 16 or 32 bits (grey, RGB, CMYK, with an alpha or
// not, samples together) as they are stored, a grey or RGB PSD or PSB of 16 or 32 bits (PC3 A3: its merged picture, a
// 32-bit file's linear light encoded by 1/2.2, its values past 1 kept); any other file as the 8-bit page reads it (its
// 8-bit values / 255).
DeepPicture deep_picture(std::string_view bytes);

// The deep picture as straight sRGB RGBA floats (0..1, HDR kept), through the colour use: keep (its numbers as sRGB's; a
// grey as (v, v, v)), embedded or assign (from that ICC profile — `icc` — to sRGB, the intent and black point compensation
// given: Little CMS in floats). CMYK and Lab through their profile (or Little CMS's default ones).
std::vector<float> srgb_floats(const DeepPicture& picture, const std::optional<std::string>& icc, std::string_view intent, bool bpc);

// The file as a CMYK, Lab, grey or multichannel page's ink pixels (its depth): a CMYK file's inks through its profile into
// the page's (black preserved) or taken as the page's when it carries none (render/ink's picture_inks for 8 bits; a TIFF of
// 16 bits with its 16 bits), a Lab or grey file's as the page's; an RGB file by the page's rules. `use`: the contents'
// colour use (keep: the CMYK file's own numbers as the page's).
core::InkPixels file_inks(std::string_view bytes, const core::PageColor& color, std::string_view use);

}  // namespace genko::render::smart
