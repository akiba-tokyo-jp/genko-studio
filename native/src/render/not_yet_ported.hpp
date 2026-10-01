#pragma once

#include <string>
#include <utility>

#include "core/error.hpp"

namespace genko::render {

// Something this build does not draw (or do) yet. Thrown instead of leaving it out silently
// (docs/cpp-migration/ARCHITECTURE.md §4a); `element` names what it is ("balloons", "tones", "effects", "prims",
// "placed", "nombre", "covers", "anim", "screen", "finish", "brush_library", …). core::Error code "not_yet_ported".
class NotYetPorted : public core::Error {
public:
    explicit NotYetPorted(std::string element)
        : core::Error("not_yet_ported", "not yet ported: " + element), element_(std::move(element)) {}

    const std::string& element() const noexcept { return element_; }

private:
    std::string element_;
};

}  // namespace genko::render
