#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/model.hpp"

// PC4 E3 (patterns@1, SPEC.md COLOR-09「パターン」): Photoshop's patterns — a PSD's global Patt, Pat2 and Pat3 blocks (an 8-,
// 16- and 32-bit document's) and a .pat file — read and written. PhotoCraft v0.2.0 crates/psd/src/patterns.rs (the
// layout, after the public Photoshop File Formats Specification's "Patterns" and "Virtual Memory Array List") and
// crates/io/src/pattern_map.rs (to and from a pattern's pixels), MIT: native/third_party/photocraft/NOTICE.md:
//
//   pattern  := version u32 (1), image mode u32, height i16, width i16, name (Unicode string), unique id (Pascal
//               string, unpadded), [indexed: 256 × RGB palette], virtual memory array list
//   VMA list := version u32 (3), length u32, rect (top, left, bottom, right i32), channel count u32, then count + 2
//               arrays (user mask: the transparency; sheet mask)
//   array    := written u32 (0: absent), length u32 (0: absent), depth u32, rect, depth u16, compression u8 (0 raw,
//               1 PackBits with the rows' counts first), data
//
// In a PSD's block each pattern has its u32 length before it and is padded to 4 bytes; a .pat file is "8BPT", version
// u16 (1), count u32, then the patterns as they are. Photoshop writes the channel count as 24 and marks only the real
// channels written: the first written arrays are the colours, the next the transparency (as ag-psd, MIT, takes them).

namespace genko::render::pat {

// One pattern as the file holds it: planar big-endian samples.
struct PsdPattern {
    std::uint32_t mode = 3;  // (PSD colour mode: 1 grey, 2 indexed, 3 RGB, 4 CMYK, 7 multichannel, 8 duotone, 9 Lab)
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;     // (UTF-8, without the closing 0; built-in ones look like "$$$/Patterns/…=Water")
    std::string id;       // (its unique id, usually a UUID)
    std::string palette;  // (indexed: 768 bytes)
    std::uint16_t depth = 8;  // (1, 8, 16 or 32)
    std::vector<std::string> channels;
    std::optional<std::string> alpha;
    friend bool operator==(const PsdPattern&, const PsdPattern&) = default;
};

// The largest edge read (Photoshop's own is far lower) and what one file's patterns may decode to in all.
inline constexpr std::uint32_t kMaxEdge = 30'000;
inline constexpr std::uint64_t kMaxFileBytes = 512ull << 20;
inline constexpr std::uint64_t kMaxDecodedBytes = 512ull << 20;

// A Patt / Pat2 / Pat3 block's data, and back; a .pat file, and back. core::Error("format") for what cannot be read
// (a file past kMaxFileBytes, patterns decoding past kMaxDecodedBytes: core::Error("too_large")).
std::vector<PsdPattern> parse_block(std::string_view data);
std::string write_block(const std::vector<PsdPattern>& patterns);
std::vector<PsdPattern> parse_file(std::string_view data);
std::string write_file(const std::vector<PsdPattern>& patterns);
// The block's key for a document of `depth` bits: Patt, Pat2 (16), Pat3 (32).
std::string_view block_key(int depth);

// A pattern of a file as a pattern's picture (core::Pattern: its id empty — the one taking it gives it —, its name the
// file's — a built-in's after "=" —, the mode and depth it has, `dpi` its pixel's size): indexed through its palette as
// RGB, duotone and multichannel as grey (the first channel), 1 bit as 8-bit grey. None for one that cannot be (no pixels,
// fewer channels than its mode has, past 8192 a side or 64 M pixels: `why` says).
struct Taken {
    core::Pattern pattern;
    core::Bytes bytes;
    std::string file_id;  // (the file's unique id)
};
std::optional<Taken> from_psd(const PsdPattern& pattern, double dpi, std::string* why = nullptr);
// A pattern's picture as the file holds one (its mode and depth; its transparency only when some pixel is not opaque),
// named `name` with the unique id `id`.
PsdPattern to_psd(const core::Pattern& pattern, std::string_view bytes, const std::string& id);
// The unique id a pattern is written with: its own id when it is a UUID (one read from a file), else one made from its
// picture's ref and its id (the same pattern, the same id).
std::string file_id(const core::Pattern& pattern);

// The PSD's global block of the book's patterns of these ids (a page's fills'): "8BIM", its key by the document's depth
// (Patt, Pat2, Pat3), its length and the patterns — each in its own mode and depth, with the unique id file_id gives it —;
// "" when there are none (a PSD of no pattern fill is written as before).
std::string global_block(const core::Document& book, const std::set<std::string>& ids, int depth);

// A name Photoshop gives a built-in pattern ("$$$/Patterns/…=Water") as it shows it ("Water"); another as it is.
std::string shown_name(std::string_view name);

}  // namespace genko::render::pat
