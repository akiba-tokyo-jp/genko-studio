#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "render/codecs/codecs.hpp"

// A file opened as a book (PC4 F-0, SPEC.md FILE-01; the user's decision 4 of 2026-10-20): a picture — and, as their
// groups add them, a PSD or PSB (F-d), a camera RAW file (F-e), a .pcraft (F-f) — becomes a book of one page held in
// memory, with no name and no folder (Document::asset_dir none: saving it asks where its .genko goes, as the untitled
// book does). The file itself is never written. Not an op (an op reads no path): the app's 開く… and the command line's
// `genko new --from` (F-b) call it with the file's bytes.
//
// The page is the picture as it is, its pixels never resampled: its paper is the picture's pixels at the resolution the
// file says (72 ppi when it says none or less, as Photoshop), its trim the paper, no bleed, no margins and no panels, one
// page bound on the left; its one layer the picture's pixels — "背景", or "レイヤー 0" for a picture with an alpha —, a
// paint layer at the page's depth:
//   - RGB of 8 bits: an 8-bit page (raster_png), its paper's grid "finest" (image-grid@1: its new layers, selections and
//     masks at its pixels' grid — design 3, 8-2 (b));
//   - RGB of 16 bits, or floats: a page in precise colour (color-model@1, 16 or 32 bits; GKCR);
//   - grey and CMYK of 8, 16 or 32 bits: a page of inks (GKIR) of that mode and depth;
// in the file's own profile where it carries one of its colours (the colour use "keep"; an sRGB profile is the 8-bit
// page's own), else the working space's ("assign": the working CMYK and grey given, else the built-in coated-cmyk and
// sgray). The page keeps what it was opened from (native_source, file-info@1).
//
// F-b fills in the rest of the opening (the colour settings' policies convert and ask, a bitmap page for a 1-bit file and
// an indexed page for a palette's, the EXIF orientation applied, the EXIF and XMP kept, File Info read, the first frame's
// note); `notes` says in Japanese what the book could not keep of the file.

namespace genko::formats {

struct OpenOptions {
    // 指定形式で開く…: the bytes read as this format (they are still checked to be its).
    std::optional<render::codecs::Format> as;
    // keep: a profile the file carries becomes the page's; discard: its numbers taken as the working space's.
    std::string profile = "keep";
    // The working spaces' ICC profiles (empty: sRGB, the built-in coated-cmyk, the built-in sgray).
    std::string working_rgb;
    std::string working_cmyk;
    std::string working_gray;
    render::codecs::Limits limits;
};

struct Opened {
    core::Document doc;
    std::vector<std::string> notes;
};

// The file of `name` (its name, no folder: the book's title is its stem) with these bytes as a book. core::Error as
// render::codecs::decode throws them ("unidentified_image", "format", "image_too_large"), render::NotYetPorted for a kind
// of file this build does not open yet (a PSD, a PSB, a RAW file and a .pcraft until their groups).
Opened open_file(std::string_view bytes, std::string_view name, const OpenOptions& options = {});

// A picture (what the codecs read) as such a book — what open_file makes of a picture, and what 新規… (an image of a
// size, mode and depth) and 複製… (a page's merged picture) make (F-b).
Opened picture_book(render::codecs::CodecImage picture, std::string_view title, const OpenOptions& options = {});

}  // namespace genko::formats
