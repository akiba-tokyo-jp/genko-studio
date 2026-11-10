#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

// Synthetic camera RAW files for the tests (PC4 F-e): PhotoCraft v0.2.0's raw/src/testgen.rs (MIT:
// native/third_party/photocraft/NOTICE.md) in C++ — a lossless JPEG (T.81 process 14) encoder, a small TIFF writer, and
// DNG (uncompressed strips, lossless-JPEG strips and tiles, CFA and LinearRaw), CR2 and TIFF/EP builders, a smooth scene
// and its mosaic through a 2×2 CFA. Written from the public specifications only, to exercise the reader (LibRaw), not to
// be camera-accurate. No camera's real RAW file is used by the tests.

namespace genko::test::rawgen {

// Encodes `samples` (raster order, `components` interleaved, values below 2^precision) as a lossless JPEG with the
// predictor (1–7) and an optional restart interval in rows.
std::string lj92_encode(const std::vector<std::uint16_t>& samples, std::size_t width, std::size_t height, std::size_t components, int precision,
                        int predictor, std::optional<std::size_t> restart_rows = std::nullopt);

// A TIFF entry's value.
struct Byte { std::vector<std::uint8_t> v; };
struct Ascii { std::string v; };
struct Short { std::vector<std::uint16_t> v; };
struct Long { std::vector<std::uint32_t> v; };
struct Rational { std::vector<std::pair<std::uint32_t, std::uint32_t>> v; };
struct SRational { std::vector<std::pair<std::int32_t, std::int32_t>> v; };
struct Undefined { std::vector<std::uint8_t> v; };
struct Blobs { std::vector<std::size_t> v; };  // (LONG offsets of these blobs)
struct Ifds { std::vector<std::size_t> v; };   // (LONG offsets of these IFDs: SubIFDs, EXIF)
struct IfdBytes { std::size_t ifd = 0; };      // (an UNDEFINED value whose bytes are this IFD: a maker note)
using Val = std::variant<Byte, Ascii, Short, Long, Rational, SRational, Undefined, Blobs, Ifds, IfdBytes>;
using Entries = std::vector<std::pair<std::uint16_t, Val>>;

struct TiffBuilder {
    bool big_endian = false;
    std::vector<Entries> ifds;
    std::vector<std::size_t> chain;  // (the main IFD chain, by index into ifds)
    std::vector<std::string> blobs;
    std::optional<std::size_t> cr2_raw_ifd;  // (a CR2 header pointing at this IFD as the raw IFD)

    std::size_t ifd(Entries entries);
    std::size_t blob(std::string data);
    std::string build() const;
};

enum class DngStorage { Strips, Lj92Tiles, Lj92Strips };

// A synthetic DNG.
struct DngSpec {
    std::size_t width = 0, height = 0;
    std::size_t samples = 1;  // 1 (CFA) or 3 (LinearRaw)
    std::vector<std::uint16_t> data;  // row-major, width × height × samples
    int bits = 16;
    std::array<std::uint8_t, 4> cfa{0, 1, 1, 2};
    DngStorage storage = DngStorage::Strips;
    std::size_t rows = 0;                     // Strips / Lj92Strips: rows a strip (0: all)
    std::size_t tile_width = 16, tile_height = 16;  // Lj92Tiles
    bool big_endian = false;
    std::vector<std::uint32_t> black{0};
    std::pair<std::uint16_t, std::uint16_t> black_repeat{1, 1};
    std::uint32_t white = 65535;
    std::optional<std::array<std::uint32_t, 4>> active_area;  // top, left, bottom, right
    std::optional<std::pair<std::array<std::uint32_t, 2>, std::array<std::uint32_t, 2>>> default_crop;  // origin, size
    std::optional<std::vector<std::uint16_t>> linearization;
    std::optional<std::pair<std::uint16_t, std::array<double, 9>>> color_matrix1, color_matrix2;  // (illuminant, XYZ → camera)
    std::optional<std::array<double, 9>> forward_matrix1;
    std::optional<std::array<double, 3>> as_shot_neutral;
    std::optional<double> baseline_exposure;
    std::uint16_t orientation = 1;
    std::string make = "Photocraft", model = "Synthetic";
    // (Genko's own: the raw IFD's compression said as this, for a DNG LibRaw does not decode; a JPEG preview in IFD0's
    // SubIFDs, as cameras write theirs)
    std::optional<std::uint16_t> compression;
    std::optional<std::string> preview_jpeg;

    // A CFA DNG with default metadata around `data` (RGGB, 16-bit, one strip).
    static DngSpec cfa_of(std::size_t width, std::size_t height, std::vector<std::uint16_t> data);
    std::string build() const;
};

// A synthetic CR2: one lossless JPEG with `components` components, cut into vertical slices, plus a Canon maker note with
// SensorInfo and ColorData.
struct Cr2Spec {
    std::size_t width = 0, height = 0;
    std::vector<std::uint16_t> data;  // sensor samples, row-major, RGGB
    int precision = 14;
    std::size_t components = 2;
    std::vector<std::size_t> slices;  // (widths; empty: no slice tag)
    std::optional<std::array<std::uint16_t, 4>> borders;  // left, top, right, bottom (inclusive)
    std::optional<std::array<std::uint16_t, 4>> wb_rggb;
    std::uint16_t orientation = 1;
    std::string model = "Canon EOS Synthetic";
    std::string build() const;
};

// A synthetic uncompressed TIFF/EP raw (NEF / ARW-like): IFD0 with Make (and Model), the CFA image in a SubIFD with
// 16-bit samples.
std::string tiff_ep(const std::string& make, const std::string& model, std::size_t width, std::size_t height, const std::vector<std::uint16_t>& data,
                    std::array<std::uint8_t, 4> cfa, int bits, Entries extra_raw_tags = {});

// A smooth, colourful scene as linear RGB in 0..1.
std::vector<std::array<float, 3>> scene(std::size_t width, std::size_t height);
// Linear RGB mosaicked through a 2×2 CFA (`cfa` row-major colours) into sensor values black + v × (white − black).
std::vector<std::uint16_t> mosaic(const std::vector<std::array<float, 3>>& rgb, std::size_t width, std::array<std::uint8_t, 4> cfa, std::uint16_t black,
                                  std::uint16_t white);

}  // namespace genko::test::rawgen
