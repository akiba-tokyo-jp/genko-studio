#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"
#include "render/pat.hpp"
#include "render/psd_descriptor.hpp"

// Photoshop brush files (.abr, Python's genko/abr.py): the sampled tips inside, as image-tip brushes. Old files
// (version 1 and 2) list the brushes one after another; newer ones (6, 7, 10) keep the tips in an "8BIM samp"
// section. Only sampled (picture) tips of 8 bits are read, as other programs do. Every number is big-endian.
//
// AbrError is Python's ValueError: core::PyValueError with its words here. Beyond Python: a version 1 / 2 record that
// points back to where it began ("a brush record points back into the file", where Python reads it again from there)
// and a file whose tips unpack to more than 2**30 pixels together (core::Error "abr_too_large", "the file's tips are
// too large to read") are refused, so a small broken file cannot unpack without end; the tips are made small one by one
// as they are read.

namespace genko::render::abr {

struct Tip {
    std::string name;  // UTF-8
    Image image;       // "L", the ink white
    int spacing = 25;  // %
};

// The pixels the tips of one file may unpack to together (32 tips of 5792 × 5792, larger than Photoshop's largest
// brushes).
inline constexpr std::int64_t kMaxPixels = std::int64_t{1} << 30;

// abr.read(data): the sampled tips. PyValueError "the file ends too early", "a tip has no size", "only 8-bit tips are
// read (this one is N-bit)", "a section does not start with 8BIM", "version N brush files are not read", "the file has
// no picture tips".
std::vector<Tip> read(std::string_view data, std::int64_t max_pixels = kMaxPixels);

// abr.tip_png(image, longest): the tip kept small (thumbnail to `longest`), as the base64 PNG a brush carries.
std::string tip_png(const Image& image, int longest = 256);

// abr.brushes_from(data, prefix): brush definitions (define_brush / the library) from the file's tips:
// {"label", "base": "gpen", "tip": "image", "tip_png", "spacing", "min_pressure": 0.3, "taper": false}.
std::vector<core::Json> brushes_from(std::string_view data, std::string_view prefix = {}, std::int64_t max_pixels = kMaxPixels);

// abr.tip_from_picture(path): a tip from any picture (dark marks on light paper, or marks on transparency).
// PyValueError "the picture could not be read"; the file's own errors as Image.open raises them.
std::string tip_from_picture(const std::filesystem::path& path);
std::string tip_from_picture(const Image& image);

// --- the whole file (PC4 E2) -------------------------------------------------------------------------------------------
//
// brush.presets.importAbr, assets:adobe-presets (SPEC.md COLOR-09「ブラシのプリセット」): the file as PhotoCraft v0.2.0's
// crates/psd/src/abr.rs reads it (MIT: native/third_party/photocraft/NOTICE.md; upstream wrote it from public
// descriptions of the layout, and so is this) — its tips of 8 and 16 bits, the computed (round) tips of version 1 and 2,
// and of version 6 and later the patterns it holds ("patt") and each brush's settings ("desc": the brushPreset
// descriptors, mapped by render/abr_map). Nothing is read past the file: every length is checked against what is left, a
// tip's size is checked (kMaxEdge) and its unpacked bytes counted against what one file may unpack to (kMaxSampleBytes)
// before it is unpacked — a file whose tips would unpack past it is refused (core::Error "too_large") —, the descriptors
// are read with render/psd_descriptor's budget, the patterns with render/pat's; a brush that cannot be read is left out
// with a warning when the rest of the file can be. The window and the CLI read the files they are given; read() and
// brushes_from() above stay the brush files of the window's older import (and of .genkobrush beside them).
//
//   v1 / v2   := version u16 (1|2), count u16, count × brush
//   brush     := type u16 (1 computed, 2 sampled), length u32, body[length]
//   computed  := misc u32, spacing u16 (%), diameter u16, roundness u16 (%), angle i16, hardness u16 (%)
//   sampled   := misc u32, spacing u16 (%), [v2: name (u32 length + UTF-16 with its 0)], anti-alias u8, short bounds
//                4 × i16, bounds 4 × i32 (top, left, bottom, right), depth u16 (8|16), compression u8 (0 raw, 1 PackBits
//                with a u16 row-length table)
//   v6+       := version u16 (6..10), subversion u16 (1|2), sections…
//   section   := "8BIM", key [4], length u32, data[length] (padded to 4 bytes)
//   "samp"    := entries: length u32 (padded to 4), Pascal uuid, skip (10 if subversion 1, 264 if 2), bounds 4 × i32,
//                depth u16, compression u8, data
//   "patt"    := patterns as a PSD's Patt block holds them (render/pat)
//   "desc"    := version u32 (16) + ActionDescriptor; its "Brsh" lists brushPreset objects

// A tip's picture: one grey plane where the largest value is full paint.
struct Sample {
    std::string id;  // (what a preset's sampledData names, v6+; else empty)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t depth = 8;  // (8 or 16)
    std::string data;         // (row by row, width × height × depth / 8 bytes; 16 bits big-endian)
    // The value of a pixel as 0..1 paint.
    float unit(std::size_t i) const;
    friend bool operator==(const Sample&, const Sample&) = default;
};

// A version 1 / 2 brush: a computed (round) tip or a sampled one.
struct LegacyBrush {
    std::string name;  // (v2 sampled brushes only)
    std::uint16_t spacing = 25;  // (% of the diameter; 0: spacing off)
    bool anti_alias = true;
    bool computed = false;
    std::uint16_t diameter = 0, hardness = 0, roundness = 100;  // (computed: pixels, %, %)
    std::int16_t angle = 0;                                     // (computed: degrees)
    Sample sample;                                              // (sampled)
    friend bool operator==(const LegacyBrush&, const LegacyBrush&) = default;
};

struct File {
    int version = 0;     // (1, 2, 6 … 10)
    int subversion = 0;  // (v6+: 1 or 2)
    std::vector<LegacyBrush> legacy;
    std::vector<Sample> samples;
    std::vector<pat::PsdPattern> patterns;
    std::vector<psd::descriptor::Object> presets;  // (the brushPreset objects of "desc", in the file's order)
    std::vector<std::string> warnings;            // (what did not stop the reading: skipped brushes, unknown sections…)
};

inline constexpr std::uint32_t kMaxEdge = 8192;                         // (a tip's largest side read; Photoshop's: 5000)
inline constexpr std::size_t kMaxBrushes = 20000;                       // (brushes read from one file)
inline constexpr std::size_t kMaxSampleBytes = std::size_t(512) << 20;  // (the tips' unpacked bytes of one file)
inline constexpr std::size_t kMaxFileBytes = std::size_t(512) << 20;    // (a file read: larger ones are refused unread)

// The file read. core::Error("format") for one that is not a brush file of these versions or holds no brush that can be
// read (the first warning says why); core::Error("too_large") for one whose tips unpack past kMaxSampleBytes.
File parse(std::string_view data, std::size_t max_sample_bytes = kMaxSampleBytes);

// Files of the same layouts (tests; upstream's write_v12, write_v6): version 1 or 2, the tips PackBits-compressed when
// `rle`; version 6 of `subversion` 1 or 2, the tips, the patterns and the brushPreset descriptors (each a class id and its
// items) listed under "Brsh". core::Error("value") for a tip of no size, past kMaxEdge or not 8 or 16 bits.
std::string write_v12(int version, const std::vector<LegacyBrush>& brushes, bool rle);
std::string write_v6(int subversion, const std::vector<Sample>& samples, const std::vector<pat::PsdPattern>& patterns,
                     const std::vector<std::pair<std::string, psd::descriptor::Writer>>& presets, bool rle);

}  // namespace genko::render::abr
