#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"

// annotations@1 (PC5 節 J、SPEC.md PROD-01「注釈」・schema-v4.md「制作の補助」): the notes placed on a page (Photoshop's
// 注釈ツール and 注釈 panel; the page's メモ — `note` — and the tickets are other things, kept as they are). The page's
// `annotations`:
//   [{"id": "an_" + 12 hex digits, "at_mm": [x, y], "popup_mm": [x, y, w, h] | null, "text": str, "author": str,
//     "color": [r, g, b], "open": bool, "modified": "YYYY-MM-DDThh:mm:ssZ" | "", "actor": "human:…" | "ai:…" | "genko"}]
// at most kAnnotationsMax, each text at most kAnnotationTextMax bytes. `actor` is who wrote it (the op's actor): an agent
// (ai:) never changes or deletes a person's note (J-c's ops). `modified` is "" when it is not known (a PSD's note with no
// date, or one this build cannot read). After PhotoCraft v0.2.0's doc/src/analysis.rs Note (MIT).
// The book names annotations@1 while a page has one. PC5 J-0 is the frame; the ops, the tool, the panel and PSD's Anno
// are J-c's (render/ops_annotations, app/notes_panel, render/psd_production).

namespace genko::core {

struct Document;

inline constexpr std::string_view kAnnotationsFeature = "annotations@1";
inline constexpr std::string_view kAnnotationsKey = "annotations";
inline constexpr std::size_t kAnnotationsMax = 1000;
inline constexpr std::size_t kAnnotationTextMax = 65536;
inline constexpr std::array<int, 3> kAnnotationColor{255, 255, 181};  // (Photoshop's pale yellow)

struct Annotation {
    std::string id;
    std::array<double, 2> at_mm{};
    std::optional<std::array<double, 4>> popup_mm;
    std::string text;
    std::string author;
    std::array<int, 3> color = kAnnotationColor;
    bool open = false;
    std::string modified;  // YYYY-MM-DDThh:mm:ssZ ("": not known)
    std::string actor = "genko";

    bool operator==(const Annotation&) const = default;
};

Json to_json(const std::vector<Annotation>& annotations);
// core::Error("value") says what is not so.
std::vector<Annotation> annotations_from_json(const Json& value);
void check_annotations(const std::vector<Annotation>& annotations);
// Is it an annotation's id ("an_" and 12 lowercase hex digits)?
bool annotation_id(std::string_view id);
// Is it a time as `modified` holds one (YYYY-MM-DDThh:mm:ssZ)?
bool annotation_time(std::string_view text);
// (PC5 J-c) Was the note written by a person (its actor one who can approve: human:…, "genko")? Then an agent (ai:) does
// not change or delete it.
bool annotation_by_person(std::string_view actor);

bool uses_annotations(const Document& doc);
void validate_annotations(const Document& doc);

}  // namespace genko::core
