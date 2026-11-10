#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/ink.hpp"
#include "core/json.hpp"
#include "core/model.hpp"
#include "render/image.hpp"

// 取り込む画像のプロファイル (PC2d, color.profileMismatch: SPEC.md COLOR-04, design 3.5; PhotoCraft v0.2.0's
// engine/src/color_cmds.rs:880-911, MIT): how an op that brings a picture's file in — put_raster, import_psd (Python's
// ops) — takes its colours, by the C++ build's own "embedded_profile":
//   "keep" (or none: Python's): the picture's numbers as they are, the page's own;
//   "convert": the numbers of the profile the file carries (proof::embedded_profile: PNG iCCP, JPEG APP2, TIFF, WebP,
//     PSD image resource 1039), brought into the page's space — a file that carries none for its colours is kept;
//   "assign": the numbers taken as those of the op's profile ("profile": a built-in id or "sha256:…" of one the book
//     holds, or "icc": its bytes in base64 — the GUI gives カラー設定's working space), brought into the page's space.
// "intent" and "bpc" say how (else the page's own: relative, black point compensation on a page with none). Into the
// page's space: on an RGB page its profile's values (sRGB on a page of none; 8 bits, as an op's picture is held), on a
// CMYK, Lab, grey or multichannel page its inks. Upstream's four answers (preserve, convert, discard, assignWorking)
// are these three and the book's own mismatch (PC1b's convert_profile, assign_profile).

namespace genko::render::import_profile {

struct Taking {
    std::string way = "keep";  // keep | convert | assign
    bool converting = false;   // (the numbers are brought into the page's space)
    core::Bytes from;          // the profile they are in (null: sRGB, Little CMS's own)
    std::string description;   // (its own description, as the op reports it)
    std::string intent = "relative";
    bool bpc = true;
};

// The op's way for a picture that carries the profile `embedded` (proof::embedded_profile of its file; none) and whose
// mode (Pillow's: "RGB", "RGBA", "P", "L", "LA", "1", "CMYK", "LAB" …) is `picture_mode`, onto `page`. Its arguments
// refused as OpError (nothing changed).
Taking taking(const core::Document& doc, const core::Page& page, const core::Json& op, const std::optional<std::string>& embedded,
              std::string_view picture_mode);

// The 8-bit picture as an RGB page holds it ("RGBA": its profile's values, sRGB on a page of none): `picture` itself
// (as "RGBA") when the way keeps it.
Image taken(const Image& picture, const Taking& way, const core::Page& page);

// The picture as a page of inks holds it, when the way converts it (else: render/ink's picture_inks as before).
core::InkPixels taken_inks(const Image& picture, const Taking& way, const core::PageColor& color);

// What the op says of it: {"way", "profile" (its description; none when kept)}.
core::Json report(const Taking& way);

}  // namespace genko::render::import_profile
