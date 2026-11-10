#pragma once

#include <string_view>
#include <vector>

// strict_gates' table of this build's ops that draw on a layer (core/command_bus.cpp): the bus looks at the book after each
// of them, and refuses one that drew on, made or took away a printed layer of a page whose name is not approved yet
// (SPEC AUTO-01「strict_gates は op の後でも確かめる」). Listed here for the tests that go round every one of them.

namespace genko::core {

std::vector<std::string_view> strict_drawing_ops();

}  // namespace genko::core
