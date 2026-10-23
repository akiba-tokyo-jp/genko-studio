#pragma once

#include <string>
#include <string_view>

// BLAKE3 (PC4 F-f, SPEC.md FILE-01): the content hash a .pcraft names its tiles and blobs by (PhotoCraft's crates/format,
// `blake3::hash(b).to_hex()`), with BLAKE3's own C implementation (native/third_party/blake3, 1.8.7).

namespace genko::render::blake3 {

// The 32-byte BLAKE3 hash of the bytes as 64 lower-case hex digits.
std::string hex(std::string_view bytes);
// 64 characters of 0-9 a-f (PhotoCraft's is_valid_hash: what a .pcraft may name a tile or a blob by).
bool is_hex_hash(std::string_view text);
// The C implementation's version ("1.8.7").
std::string version();

}  // namespace genko::render::blake3
