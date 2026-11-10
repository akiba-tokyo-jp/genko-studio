#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"

// PC2a (SPEC.md COLOR-04, schema-v4.md「補正（adjustments@1）」: the 3D lookup table of color_lookup): the creative LUT
// files a person gives (.cube — Adobe / Resolve, a 1D one spread to 33³ —, .3dl — Lustre —, .look — SpeedGrade), the
// eight looks PhotoCraft generates (CC0), a table as a book holds it (the asset assets/<ab>/<64>.gklut), a .cube written
// back, and a colour looked up (trilinear or tetrahedral, PhotoCraft's ordered dither). PhotoCraft v0.2.0
// cms/lutfile.rs and compose/adjust.rs (lut3d_sample, bayer4); attribution: native/third_party/photocraft/NOTICE.md.
//
// A table: `size`³ RGB triplets in 0..1 (as given: a file may hold values beyond), red varying fastest
// (((b·size + g)·size + r)·3).

namespace genko::core::lut3d {

inline constexpr std::size_t kMaxSize = 129;                     // (PhotoCraft's MAX_SIZE)
// (a file's text an op takes: a .cube of 129³ with six decimals, about 56 MiB, within it)
inline constexpr std::size_t kMaxFileBytes = 64 * 1024 * 1024;
inline constexpr std::string_view kSuffix = ".gklut";

struct Table {
    std::string title;
    std::size_t size = 0;
    std::vector<float> data;
};

// The identity of edge `size`, and `f` sampled on a `size`³ grid (each value held to 0..1).
Table identity(std::size_t size);
Table from_fn(std::string title, std::size_t size, const std::function<std::array<float, 3>(std::array<float, 3>)>& f);

// A file's table, its format by the name's extension (.3dl, .look; a text starting with "<" is a .look; else .cube).
// core::Error("value") with PhotoCraft's words for one it cannot read (a size beyond 2..129, entries missing or more,
// a value not finite, …). Its numbers are read as Rust reads them, whatever the C locale (a point for the decimal).
Table parse(std::string_view file_name, std::string_view text);
Table parse_cube(std::string_view text);
Table parse_3dl(std::string_view text);
Table parse_look(std::string_view text);
// The table as a .cube file (its TITLE when it has one, LUT_3D_SIZE, each entry with 6 decimals and a point, whatever
// the C locale).
std::string write_cube(const Table& table);
// A value as a .cube writes it: 6 decimals and a point, whatever the C locale (printf's "%.6f" in the C locale).
void append_fixed6(std::string& out, double v);

// PhotoCraft's generated looks (procedural, CC0): their ids and labels, in its order; a look's table (33³, made once).
struct Look {
    std::string_view id;
    std::string_view label;
};
const std::array<Look, 8>& looks();
const Table* builtin(std::string_view id);

// The table at a colour (each channel held to 0..1 first), trilinear or tetrahedral (PhotoCraft's lut3d_sample).
std::array<double, 3> sample(const Table& table, const std::array<double, 3>& rgb, bool tetrahedral);
// PhotoCraft's ordered dither: the 4×4 Bayer offset in -0.5..0.5 of the page's pixel (x, y).
double bayer4(int x, int y);

// The table as a book holds it: "GKLU", version 1 (u8), its size (u8), then zlib's stream of size³ × 3 float32 (little
// endian, red fastest). The same table is always the same bytes (the same asset). decode: core::Error("value") for
// bytes that are not one (another magic or version, a size beyond 2..129, entries missing or more, a value not finite).
std::string encode(const Table& table);
Table decode(std::string_view bytes);
// A decoded table by its bytes' ref, kept a few at a time (a table is drawn many times; 129³ is about 26 MB). None
// without its bytes: a table is always the book's (Document::luts) — never one another book decoded before.
std::shared_ptr<const Table> decoded(const std::string& ref, const Bytes& bytes);

// A correction's lookup table into the book an op changes (add_layer, set_layer, set_adjustment, import_psd: a
// color_lookup with a `lut`): the book's own, or one the op just made of a file's text or read from a PSD (`made`, by
// ref). OpError when neither holds it (a table of another book: its file's text is given as `data`).
void keep_luts(std::map<std::string, Bytes>& luts, const std::map<std::string, Bytes>& made, const Json& adjust);
// The tables a book's (or a page's) color_lookup corrections use (their "lut" refs, each once, in their order): what the
// writer writes, what a page brings into another book (import_pages).
std::vector<std::string> refs_of(const Document& doc);
std::vector<std::string> refs_of(const Page& page);

}  // namespace genko::core::lut3d
