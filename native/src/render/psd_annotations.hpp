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
// page (ReadContext::onto), their author "genko" (a person's: the file's). Writing: while the page's notes are as read, the
// block's own bytes (psd_production's mark); else the page's notes in the file's pixels and the kept sound notes.

namespace genko::render::psd_annotations {

inline constexpr std::string_view kKey = "Anno";

// One note of a block as the file holds it (the file's pixels), or a note of another kind kept as its bytes.
struct FileNote {
    bool is_text = true;                 // (a text note, txtA; else another kind kept as its bytes)
    std::string raw;                     // (not a text note: its bytes, its length first)
    bool open = false;
    std::array<double, 2> at_px{};       // the icon's top left (x, y)
    std::array<double, 4> popup_px{};    // the box: left, top, right, bottom (all 0: none)
    std::array<int, 3> color = core::kAnnotationColor;
    std::string author;                  // (UTF-8)
    std::string modified;                // YYYY-MM-DDThh:mm:ssZ, "" none
    std::string text;                    // (UTF-8, lines ending in \n)
};

// The block's notes (none: not one this build reads).
std::optional<std::vector<FileNote>> parse(std::string_view data);
// A block of these notes.
std::string block(const std::vector<FileNote>& notes);
// "D:YYYYMMDDhhmmss…" → YYYY-MM-DDThh:mm:ssZ ("" for one it cannot read), and back.
std::string utc_of_pdf_date(std::string_view pdf);
std::string pdf_date_of(std::string_view utc);
// The file's notes as the page's (the file's pixels → mm by `px_to_mm`: x, y → mm), their ids new and unique in `taken`.
std::vector<core::Annotation> notes_of(const std::vector<FileNote>& notes, const std::array<double, 6>& px_to_mm, const std::vector<core::Annotation>& taken);

// psd_production's row.
bool read(core::Page& page, std::string_view data, const psd_production::ReadContext& context);
std::optional<std::string> write(const core::Page& page, const psd_production::WriteContext& context);
std::string fingerprint(const core::Page& page, const core::Document* doc);

}  // namespace genko::render::psd_annotations
