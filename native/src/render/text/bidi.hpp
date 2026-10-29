#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// PC5 G-b (the user's decision 9 of 2026-10-28, SPEC.md TEXT-02「右から左」): right-to-left text set across — the Unicode
// bidirectional algorithm (UAX #9) of SheenBidi (Apache-2.0, native/third_party/sheenbidi) over a paragraph of the
// OpenType typesetting (render/text/lettering): each letter's embedding level, and a row's runs in the order they are
// drawn, left to right (UAX #9 L1 and L2). Down (縦組み) is Japanese alone: the columns never take it. And each letter's
// script (SheenBidi's script runs: the Unicode scripts, a common or inherited letter taking its neighbours'), which
// HarfBuzz is told run by run.
//
// Texts are code points (std::u32string), places in them code points.

namespace genko::render::text {

// A paragraph's levels (even: left to right, odd: right to left) and its base level (0 or 1), as SheenBidi finds them for
// the paragraph's direction: "auto" (its first strong letter's; left to right without one), "ltr" or "rtl".
class BidiParagraph {
public:
    BidiParagraph();
    BidiParagraph(std::u32string text, std::string_view direction);
    ~BidiParagraph();
    BidiParagraph(BidiParagraph&&) noexcept;
    BidiParagraph& operator=(BidiParagraph&&) noexcept;
    BidiParagraph(const BidiParagraph&) = delete;
    BidiParagraph& operator=(const BidiParagraph&) = delete;

    std::uint8_t base() const { return base_; }
    bool rtl() const { return (base_ & 1) != 0; }
    const std::vector<std::uint8_t>& levels() const { return levels_; }
    // Is any letter right to left (a level that is odd)?
    bool mixed() const;

    // The runs of the row [start, end) of the paragraph in the order they are drawn, left to right: each a stretch of
    // the paragraph's letters [start, end) at one level (its letters drawn right to left when the level is odd). The
    // row's whitespace at its end takes the paragraph's level (L1).
    struct Run {
        std::size_t start = 0;
        std::size_t end = 0;
        std::uint8_t level = 0;
    };
    std::vector<Run> visual_runs(std::size_t start, std::size_t end) const;

private:
    struct State;
    std::unique_ptr<State> state_;
    std::vector<std::uint8_t> levels_;
    std::uint8_t base_ = 0;
};

// Each letter's script as an ISO 15924 tag ("Latn", "Arab", "Hani", …; "Zyyy" for one that has none of its own after
// SheenBidi's resolution).
std::vector<std::string> scripts_of(std::u32string_view text);

// SheenBidi's version ("2.9.0").
std::string sheenbidi_version();

}  // namespace genko::render::text
