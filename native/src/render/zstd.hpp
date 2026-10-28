#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// zstd (PC4 F-f, SPEC.md FILE-01): a .pcraft's tiles and blobs are zstd frames (PhotoCraft's crates/format: ruzstd at its
// fastest level). Unpacking never makes more than it is allowed to, whatever a frame's header says or its data would
// unpack to: a frame that says (or turns out) larger is refused before or while it is unpacked, never after (a zstd bomb),
// and a frame asking for a window beyond zstd's default limit (128 MiB) is refused.

namespace genko::render::zstd {

// The bytes as one zstd frame at `level` (1: the fastest, as PhotoCraft writes them).
std::string compress(std::string_view bytes, int level = 1);

// A frame that must unpack to exactly `expected` bytes (a .pcraft's tile: its size is the tile's format's). core::Error
// ("format") for a frame that is broken, says another size or unpacks to more or fewer bytes; no more than `expected` bytes
// are ever made.
std::string decompress_exact(std::string_view frame, std::size_t expected);

// A frame of at most `max` bytes unpacked (a .pcraft's blob, whose size the manifest does not say). core::Error("format")
// for a broken frame; core::Error("image_too_large") for one that says or turns out larger than `max` — refused before
// it is unpacked when its header says so, and as soon as it passes `max` otherwise.
std::string decompress_bounded(std::string_view frame, std::size_t max);

// libzstd's version ("1.5.5").
std::string version();

}  // namespace genko::render::zstd
