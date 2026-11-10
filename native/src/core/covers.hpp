#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Covers (表紙・裏表紙・カバー・帯): pages marked page.extra["cover"] = {"kind": front | back | jacket | obi,
// "spine_mm", "flap_mm", "height_mm"} (Python's genko/covers.py). Covers have no nombre and do not count as pages; the
// book preview and the exports put them first and last. A jacket (カバー) is one wide sheet with both covers, the spine
// between them and the flaps (袖) at its ends, laid out as it prints: for a book bound on the right (右綴じ)
// [flap][front][spine][back][flap], on the left the other way round; a band (帯) is as wide and as tall as the band.

namespace genko::core {

// covers.KINDS: front, back, jacket, obi (in this order).
bool is_cover_kind(std::string_view kind);
// covers.WRAPS: the kinds that are one wide sheet round the book (jacket, obi).
bool is_wrap_kind(std::string_view kind);

// The paper of a cover (covers.spec_for): the book's own for the front and back; for a jacket (カバー) or an obi
// (帯), one wide sheet with both covers, the spine and the flaps, with the book's bleed and paper allowance.
PageSpec spec_for(const PageSpec& book, const Json& cover);

// covers.cover_of: the page's cover entry when it is one (kind front, back, jacket or obi), else null.
const Json* cover_of(const Page& page);

// covers.is_cover
inline bool is_cover(const Page& page) { return cover_of(page) != nullptr; }

// One part of a jacket or a band across its page (covers.folds): from x0 to x1 (mm, rounded to 3 places), and its name
// (袖, 表紙, 背, 裏表紙).
struct Fold {
    double x0 = 0;
    double x1 = 0;
    std::string name;
};

// covers.folds(page, binding): the parts of a jacket or a band from the left, so where it folds (none for a page that
// is not one, and none of no width). `binding` is "right" (右綴じ) or anything else (左綴じ). Python's ValueError
// and TypeError (PyValueError, PyTypeError) for a spine or flap that is not a number.
std::vector<Fold> folds(const Page& page, std::string_view binding = "right");

// covers.pages_in_order: the book as it is read: the jacket (or else the front cover), the pages that are not covers,
// the back cover.
std::vector<const Page*> pages_in_order(const Document& doc);

// covers.reading_order: (page, part) as a reader turns through the book, part "front", "page" or "back". A jacket
// gives the front and, when the book has no back cover of its own, the back.
std::vector<std::pair<const Page*, std::string>> reading_order(const Document& doc);

// covers.file_stem: the part of an exported file's name for this page: p003, or cover_front / cover_back /
// cover_jacket / cover_obi. PyValueError for a page number that is a float (Python's f"{index:03d}").
std::string file_stem(const Page& page);

}  // namespace genko::core
