#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "render/psd_descriptor.hpp"

// PSD-01: Photoshop's text layer structures, written by the editable PSD (formats/psd_export.cpp) and read back by
// render::psd::File (the structure checks): a text layer's TySh block — its transform, its TxLr descriptor (the text,
// its orientation) and the text engine's EngineData (the characters' style runs, the paragraphs, the fonts) — and the
// layer effects' lfx2 block holding a single-colour stroke (FrFX). The descriptors and EngineData they are made of are
// written as Photoshop writes them (and as psd-tools and Krita read them); the reader takes what this writer writes and
// files of the same shape. Font files are never put in a PSD: a font is named (its PostScript name), never embedded.

namespace genko::render::psd {

// A character style run's look (EngineData's StyleSheetData). Sizes and distances are document pixels (Photoshop
// shows them in points at the file's resolution).
struct TextStyle {
    std::string font;                // Font: its PostScript name (FontSet)
    double size = 12.0;              // FontSize
    bool faux_bold = false;          // FauxBold
    bool faux_italic = false;        // FauxItalic
    std::array<int, 3> fill{0, 0, 0};  // FillColor (RGB, 0–255)
    double tracking = 0.0;           // Tracking: 1/1000 em
    double leading = 0.0;            // Leading (AutoLeading off): baseline to baseline (down: column to column)
    double horizontal_scale = 1.0;   // HorizontalScale
    double vertical_scale = 1.0;     // VerticalScale
    int baseline_direction = 2;      // BaselineDirection: 1 upright, 2 mixed (Latin laid on its side down), 3 縦中横
    int run_alignment = 2;           // StyleRunAlignment: 0 em box bottom (left), 2 em box centre, 3 roman baseline
    // (color-mode@1) FillColor as a CMYK document's inks (Type 2: C, M, Y, K 0..1), when it is one; fill then the RGB
    // they stand for (K alone: its grey)
    std::optional<std::array<double, 4>> fill_cmyk;

    bool operator==(const TextStyle&) const = default;
};

// `length` characters (UTF-16 code units) in one style, or in one paragraph (Justification: 0 left / top, 1 right /
// bottom, 2 centre, 3 justified with the last line to the left / top, 6 justified, the last line too).
struct StyleRun {
    std::size_t length = 0;
    TextStyle style;

    bool operator==(const StyleRun&) const = default;
};
struct ParagraphRun {
    std::size_t length = 0;
    int justification = 0;

    bool operator==(const ParagraphRun&) const = default;
};

// A text layer (TySh). `text` is the layer's characters, "\r" between its lines (the text engine's own trailing "\r"
// is not part of it); `runs` and `paragraphs` cover the text and that trailing "\r". `transform` maps the text's own
// space (pixels, its anchor at the origin) to the document's: x' = xx·x + yx·y + tx, y' = xy·x + yy·y + ty. Point text
// sets its lines from the anchor (across: the first line's baseline; down: the first column's middle line, at its top
// — or its middle or foot as the paragraphs are aligned); paragraph (area) text (`box`, in the text's space) fills its
// box. `bounds`: where the letters lie in the text's space (left, top, right, bottom).
struct TextLayer {
    std::u16string text;
    bool vertical = false;
    std::array<double, 6> transform{1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    std::vector<StyleRun> runs;
    std::vector<ParagraphRun> paragraphs;
    std::optional<std::array<double, 4>> box;
    std::array<double, 4> bounds{0.0, 0.0, 0.0, 0.0};
    // (read back only) the EngineData's own copy of the text (with its trailing "\r")
    std::u16string engine_text;
    // The text's warp (Warp Text, PC3 C-c), written and read back: Photoshop's style ("warpNone", "warpArc", "warpFlag"…),
    // its bend and its horizontal and vertical distortions (per cent), along or across (warpRotate Vrtc)
    std::string warp = "warpNone";
    double warp_value = 0.0;
    double warp_perspective = 0.0;
    double warp_perspective_other = 0.0;
    bool warp_vertical = false;

    // The style of the character at `at` (UTF-16 code units); nothing past the runs.
    const TextStyle* style_at(std::size_t at) const;
};

// The TySh block's payload for a text layer (version 1, text version 50, the TxLr descriptor with its EngineData, the
// warp descriptor: warpNone unless the layer has one). Fonts are named in the order the runs first use them, after Photoshop's AdobeInvisFont.
std::string tysh(const TextLayer& layer);
// A TySh block's payload read back: core::Error("format") when it is not one this reader takes.
TextLayer read_tysh(std::string_view payload);

// A single-colour stroke around a layer's pixels (the layer effect 「境界線」, FrFX): `size` pixels outside (or inside,
// centre) the letters, in `rgb`, at `opacity` per cent.
struct Stroke {
    Stroke() = default;
    Stroke(double size_, std::array<int, 3> rgb_, std::string position_ = "outside", double opacity_ = 100.0, bool enabled_ = true)
        : size(size_), rgb(rgb_), position(std::move(position_)), opacity(opacity_), enabled(enabled_) {}

    double size = 0.0;
    std::array<int, 3> rgb{255, 255, 255};
    std::string position = "outside";  // outside | inside | center
    double opacity = 100.0;
    bool enabled = true;
    // (color-mode@1) the colour as a CMYK document's (Clr  CMYC: C, M, Y, K %) or a Lab one's (LbCl: L, a, b), when it
    // is one's (rgb then what a CMYK colour stands for; a Lab one's left as it is)
    std::optional<std::array<double, 4>> cmyk;
    std::optional<std::array<double, 3>> lab;

    bool operator==(const Stroke&) const = default;
};
// The lfx2 block's payload holding the stroke (and the effects switched on).
std::string lfx2(const Stroke& stroke);
// Its stroke read back (none when it has none): core::Error("format") when it is not an lfx2 block.
std::optional<Stroke> read_lfx2(std::string_view payload);

// (UTF-8 ↔ UTF-16, the PSD's text: utf16 and utf8_of, render/psd_descriptor.hpp)

}  // namespace genko::render::psd
