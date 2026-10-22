#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/model.hpp"
#include "render/codecs/codecs.hpp"

// A file opened as a book (PC4 F-0, F-b; SPEC.md FILE-01; the user's decision 4 of 2026-10-20): a picture — and, as their
// groups add them, a PSD or PSB (F-d), a camera RAW file (F-e), a .pcraft (F-f) — becomes a book of one page held in
// memory, with no name and no folder (Document::asset_dir none: saving it asks where its .genko goes, as the untitled
// book does). The file itself is never written. Not an op (an op reads no path): the app's 開く… and the command line's
// `genko new --from` call it with the file's bytes.
//
// The page is the picture as it is, its pixels never resampled: its paper is the picture's pixels at the resolution the
// file says (72 ppi when it says none or less, as Photoshop), its trim the paper, no bleed, no margins and no panels, one
// page bound on the left; its one layer the picture's pixels — "背景", or "レイヤー 0" for a picture with an alpha —, a
// paint layer at the page's depth:
//   - RGB of 8 bits: an 8-bit page (raster_png), its paper's grid "finest" (image-grid@1: its new layers, selections and
//     masks at its pixels' grid — design 3, 8-2 (b));
//   - RGB of 16 bits, or floats: a page in precise colour (color-model@1, 16 or 32 bits; GKCR);
//   - grey and CMYK of 8, 16 or 32 bits: a page of inks (GKIR) of that mode and depth;
//   - a picture of a colour table (a PNG or TIFF of a palette, a GIF, a BMP of 8 bits or less): an indexed page of its
//     colours (to_indexed's exact table); a picture of one bit of grey: a bitmap page (to_bitmap at its threshold);
// in the file's own profile where it carries one of its colours (the colour use "keep"; one that shows its numbers as
// sRGB does — Little CMS says so of its colours, render::proof::same_as_srgb — is the 8-bit page's own), else the working
// space's ("assign": the working CMYK and grey given, else the built-in coated-cmyk and sgray; an RGB file whose EXIF says
// sRGB, unless the EXIF's profile tag is ignored, is sRGB). The page keeps what it was opened from (native_source,
// file-info@1): the file's EXIF block (its orientation, applied to the pixels when opened — a TIFF's is applied by its
// reader —, set to 1) and XMP packet as assets, the XMP's File Info as the book's (file_info, its packet kept with it).
// `notes` says in Japanese what the book could not keep of the file (the first frame only of an animation or of a TIFF of
// pages, the orientation applied, a profile not of its colours). The colour settings' policies of the window (keep,
// convert, discard, ask) are the window's, after it is opened (app/main_window_color.cpp color_policy_on_open).

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
    // 環境設定 → ファイル管理 → EXIF のプロファイルのタグを無視: an RGB file with no profile whose EXIF says sRGB takes the
    // working RGB all the same.
    bool ignore_exif_profile = false;
    render::codecs::Limits limits;
};

struct Opened {
    core::Document doc;
    std::vector<std::string> notes;
    // (open_file) the file carried no profile of its colours (nor an EXIF that says sRGB): its page took the working one —
    // the window asks, when カラー設定 says to ask about a missing profile (Photoshop's "Missing Profiles: Ask When Opening")
    bool profile_missing = false;
};

// The file of `name` (its name, no folder: the book's title is its stem) with these bytes as a book. core::Error as
// render::codecs::decode throws them ("unidentified_image", "format", "image_too_large"), render::NotYetPorted for a kind
// of file this build does not open yet: "psd_file" (a PSD or PSB: F-d), "raw_file" (a camera RAW file: F-e), "pcraft_file"
// (a .pcraft: F-f), "image_format" (a picture of a format the codecs do not read yet: F-a).
Opened open_file(std::string_view bytes, std::string_view name, const OpenOptions& options = {});

// A picture (what the codecs read) as such a book — what open_file makes of a picture, and what 新規… (an image of a
// size, mode and depth) and 複製… (a page's merged picture) make.
Opened picture_book(render::codecs::CodecImage picture, std::string_view title, const OpenOptions& options = {});

// 新規… → 画像 (file.new; PhotoCraft's commands.rs new_document): a book of one page in memory of `width` × `height`
// pixels at `dpi` (72..100000), its mode (rgb, gray, cmyk, lab, bitmap) and depth (8, 16, 32; bitmap 8, lab 8 or 16), its
// one layer "背景" filled with the background (white, black, #rrggbb — the window gives 背景色 so — or transparent: an
// empty "レイヤー 1"), in `profile` (a built-in profile's id, alias or sha256; empty: the working space's). Its 8-bit RGB
// page at its pixels' grid as an opened picture's. core::Error("value") for what it cannot make.
struct NewImage {
    std::string title;
    std::int64_t width = 0;
    std::int64_t height = 0;
    double dpi = 72;
    std::string mode = "rgb";
    int depth = 8;
    std::string background = "white";
    std::string profile;
};
Opened new_image(const NewImage& image, const OpenOptions& options = {});

// 画像 → 複製… (image.duplicate; PhotoCraft's image_cmds.rs duplicate): the page as a book of its own in memory, named
// `title` — its layers, lines and the book's settings as they are (its pictures read from where the book's are:
// Document::asset_dir), or, `merged_only`, one layer of its merged picture at its depth and resolution (page_picture at
// its dpi) as an opened picture is.
Opened duplicate_page(const core::Document& doc, std::size_t page, std::string_view title, bool merged_only, const OpenOptions& options = {});

}  // namespace genko::formats
