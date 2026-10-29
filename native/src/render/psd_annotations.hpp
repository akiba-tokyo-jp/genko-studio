#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/annotations.hpp"
#include "core/model.hpp"
#include "render/psd_production.hpp"

// PC5 J-c (annotations@1, SPEC.md PROD-01「注釈」, FILE-01「PSD・PSB」): a PSD's notes — the global block Anno, Adobe's
// "Annotations (Photoshop 6.0)" — as the page's, and back: PhotoCraft v0.2.0's crates/io/src/annotations_map.rs (MIT,
// native/third_party/photocraft/NOTICE.md). (J-d1 adds the measurement scale, resource 1074, to this file.)
//
// The block: u16 2, u16 1, u32 count, then each note — u32 its length (itself counted), its kind (txtA a text note, sndA a
// sound note), open u8, flags u8, u16 optional blocks, the icon's rect and the box's (i32 top, left, bottom, right: the
// file's pixels), its colour (u16 space, 4 × u16: RGB 0..65535, HSB, CMYK inverted, grey 0..10000), its author, name and
// date as Pascal strings padded to an even length (one byte a character: Latin-1 here, "?" for any other), then u32 the
// length of its data (itself counted), txtC, u32 its size and its text (UTF-16BE after a byte-order mark, lines ending in
// CR). A sound note is not the page's: it is kept in the block's kept bytes and written back after the page's text notes
// whenever the block is written anew (WriteContext::kept_global). The date is a PDF date ("D:YYYYMMDDhhmmss" and the zone)
// read as UTC (the page's `modified`; "" when it is none) and written "D:YYYYMMDDhhmmssZ".
// Reading: the page's notes when it has none yet and keeps no earlier PSD's blocks; placed through the file's canvas on the
// page (ReadContext::onto) and pulled inside the paper, written by the reader (annotations::ReadingActor: the op's actor —
// an agent's import its own —, else "genko", a person's: the file's). Writing: while the page's notes are as read, the
// block's own bytes (psd_production's mark); else the page's notes in the file's pixels and, after them, of the kept block
// the sound notes (a block read: its text notes are the page's) or every note as it was (a block not read — the review's
// P1-1: a book of before J-c, a block this build could not read —; none of the page's: the kept block as it is).

namespace genko::render::psd_annotations {

inline constexpr std::string_view kKey = "Anno";

// One note of a block as the file holds it (the file's pixels), or a note of another kind kept as its bytes.
struct FileNote {
    bool is_text = true;                 // (a text note, txtA; else another kind kept as its bytes)
    // (the review's P2-2) where it is in the block it was read from — its length first — and whether block() writes those
    // bytes (a note of another kind always; a text note when it is the kept block's, not the page's)
    std::size_t at = 0, length = 0;
    bool as_read = false;
    bool open = false;
    std::array<double, 2> at_px{};       // the icon's top left (x, y)
    std::array<double, 4> popup_px{};    // the box: left, top, right, bottom (all 0: none)
    std::array<int, 3> color = core::kAnnotationColor;
    std::string author;                  // (UTF-8)
    std::string modified;                // YYYY-MM-DDThh:mm:ssZ, "" none
    std::string text;                    // (UTF-8, lines ending in \n)
};

inline constexpr std::size_t kEntriesMax = 4096;  // (notes of every kind a block holds that this build reads; more: kept as it is)

// The block's notes (none: not one this build reads — broken, or more than kEntriesMax).
std::optional<std::vector<FileNote>> parse(std::string_view data);
// A block of these notes (a note of another kind, or one `as_read`, its bytes in `source` — the block it was read from).
std::string block(const std::vector<FileNote>& notes, std::string_view source = {});
// "D:YYYYMMDDhhmmss…" → YYYY-MM-DDThh:mm:ssZ ("" for one it cannot read), and back.
std::string utc_of_pdf_date(std::string_view pdf);
std::string pdf_date_of(std::string_view utc);
// The file's notes as the page's (the file's pixels → mm by `px_to_mm`: x, y → mm), their ids new and unique in `taken`.
std::vector<core::Annotation> notes_of(const std::vector<FileNote>& notes, const std::array<double, 6>& px_to_mm, const std::vector<core::Annotation>& taken);

// psd_production's row.
bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);
std::string fingerprint(const core::Page& page, const core::Document* doc);

// --- PC5 J-d1 (measure@1, SPEC.md PROD-01「計測」): the measurement scale — image resource 1074, a versioned descriptor
// (version 16) of pixelLength, logicalLength (doubles) and logicalUnits (text): Photoshop's scripting names, as PhotoCraft
// v0.2.0's annotations_map.rs:279-330 reads them (its four-character ids PxlL, LglL, LglU too, and one object deep) — and
// the count, resource 1080 (its form not public): kept as it was while the page has no count marks of its own, not written
// once it has (they would say otherwise; PhotoCraft's). The scale's pixel_length is the file's pixels in the file and the
// page's in the book (each through the file's canvas on the page). Read when the page has no scale yet and keeps no earlier
// PSD's blocks; a scale of 1 pixels = 1 pixels is none (the block kept as it was); written while the page has one — the
// block's own bytes while it is the one read (psd_production's mark).

inline constexpr int kScaleResource = 1074;
inline constexpr int kCountResource = 1080;

// The scale a 1074 holds (none: not one this build reads).
std::optional<core::MeasurementScale> scale_of(std::string_view data);
// A 1074 of the scale (even length).
std::string scale_block(const core::MeasurementScale& scale);
bool read_scale(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write_scale(const core::Page& page, const psd_production::WriteContext& context);
std::string scale_fingerprint(const core::Page& page, const core::Document* doc);
// 1080's row: the kept count not written while the page has its own marks.
bool counts_own(const core::Page& page);

}  // namespace genko::render::psd_annotations
