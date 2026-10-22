#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// psd-keep@1 (PC4 F-d; core/psd_keep.hpp, SPEC.md FILE-01「PSD・PSB」): the bytes of what a PSD or PSB held that this build
// does not read, as import_psd keeps them and the PSD writer (formats/psd_export) puts them back — a tagged block as the
// file format lays it out for a PSD or a PSB, and the patterns of a pattern block (Patt, Pat2, Pat3: each its u32 length,
// the pattern, padding to 4 bytes; render/pat.hpp) told apart by their unique ids, so that the patterns a fill took into
// the book are not kept twice and the book's own are written with the file's in one block.

namespace genko::render::psd_keep {

// A tagged block: its signature (8BIM, 8B64), key, length — 8 bytes in a PSB for the keys of core::psb_long_key, else 4 —
// and payload. A layer record's block is not padded (its length is all there is: psd-tools reads it so), a global one is
// padded to 4 bytes after its length (as the writer and psd-tools read them).
std::string tagged_block(std::string_view signature, std::string_view key, std::string_view payload, bool psb, bool global);

struct PatternEntry {
    std::string id;           // (its unique id; "" for one that does not say)
    std::string_view bytes;   // (the pattern, without its length and padding)
};
// The patterns of a pattern block's payload in their order (what cannot be one ends the list).
std::vector<PatternEntry> pattern_entries(std::string_view payload);
// The payload of these patterns.
std::string pattern_payload(const std::vector<PatternEntry>& entries);

}  // namespace genko::render::psd_keep
