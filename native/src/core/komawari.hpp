#pragma once

#include <string_view>

#include "core/model.hpp"

// komawari@1 (コマ割り K1, docs/cpp-migration/SPEC.md LAYOUT-01, schema-v4.md「コマの重ね」): a panel's "over" (Frame::over:
// it lies over the others — 差し込み・重ね). The book names the feature while a panel of it has it.

namespace genko::core {

inline constexpr std::string_view kKomawariFeature = "komawari@1";

// Whether a panel of a page the book writes lies over the others (core/frames.cpp).
bool uses_over_panels(const Document& doc);

}  // namespace genko::core
