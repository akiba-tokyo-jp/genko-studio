#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// PC4 節 D6 (SPEC.md COLOR-08「フィルター」: レンズ補正・広角補正): what a camera's EXIF says of a shot that the lens
// filters use — PhotoCraft v0.2.0's algo/src/exif.rs (MIT, native/third_party/photocraft/NOTICE.md), a minimal reader of
// the TIFF-structured EXIF block (II*\0 / MM\0*, a JPEG's "Exif\0\0" before it skipped) as a file holds it
// (render/codecs read_metadata gives it; a page opened from a file keeps it: file-info@1's native_source). Bytes that are
// cut short or broken give what was read before them, never an error. Tag numbers: EXIF 2.32 (CIPA DC-008).

namespace genko::core::exif {

struct Camera {
    std::optional<std::string> make;
    std::optional<std::string> model;
    std::optional<std::string> lens;
    std::optional<double> exposure_time;      // seconds
    std::optional<double> f_number;
    std::optional<double> iso;
    std::optional<double> focal_length;       // mm (0x920A)
    std::optional<double> focal_length_35mm;  // the 35 mm-equivalent focal length, mm (0xA405; 0 is none)

    bool operator==(const Camera&) const = default;
};

// A TIFF structure — an EXIF block, a TIFF file — read in its byte order: the one reader of both (render/codecs reads a
// file's and its EXIF's orientation and colour space with it, PC4 D6's review P3-4). Every read stays within its bytes
// (none past them, nothing thrown).
class Tiff {
public:
    // II*\0 or MM\0* first (ok() false for anything else: every read none).
    explicit Tiff(std::string_view bytes);
    bool ok() const { return ok_; }
    bool little() const { return le_; }
    std::string_view bytes() const { return b_; }
    bool has(std::size_t at, std::size_t n) const { return at <= b_.size() && n <= b_.size() - at; }
    std::optional<std::uint16_t> u16(std::size_t at) const;
    std::optional<std::uint32_t> u32(std::size_t at) const;
    // The first IFD's place (0: none).
    std::size_t first_ifd() const;
    // An IFD's entry: its tag, type, count and where its 4-byte value field is.
    struct Entry {
        std::uint16_t tag = 0, type = 0;
        std::uint32_t count = 0;
        std::size_t field = 0;
    };
    // The IFD's entries (up to the first cut short), and the one of `tag`.
    std::vector<Entry> ifd(std::size_t at) const;
    std::optional<Entry> entry(std::size_t ifd, std::uint16_t tag) const;
    // Where an entry's values are: its field when they fit in 4 bytes, else where the field points.
    std::optional<std::size_t> data_at(const Entry& e) const;
    // A SHORT or LONG entry's first value.
    std::optional<std::uint32_t> whole(const Entry& e) const;
    // A SHORT, LONG, RATIONAL or SRATIONAL entry's first value (a zero denominator: none).
    std::optional<double> number(const Entry& e) const;
    // An ASCII entry's text, its zeros and blanks at the ends trimmed (none left: none).
    std::optional<std::string> text(const Entry& e) const;
    // A BYTE, ASCII or UNDEFINED entry's bytes (all within the block).
    std::optional<std::string_view> bytes_of(const Entry& e) const;
    // The IFD after it (0: none).
    std::size_t next_ifd(std::size_t ifd) const;

private:
    std::string_view b_;
    bool le_ = true;
    bool ok_ = false;
};

// What the EXIF block says (IFD0 and the Exif IFD it points to).
Camera read(std::string_view exif);
// A little-endian EXIF block saying `camera` (the tests' and synthetic files').
std::string build(const Camera& camera);

}  // namespace genko::core::exif
