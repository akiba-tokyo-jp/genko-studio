#pragma once

#include <string>

#include "core/json.hpp"
#include "core/pynum.hpp"

// Python's format specs the studio's words use (f"{x:.2f}", f"{x:.0%}", f"{x:g}") and str() of a JSON value.

namespace genko::service {

// format(x, ".Nf"): x rounded at n places from its exact value, ties to even ("-0" for minus zero, as Python).
std::string fixed(double x, int places);
inline std::string fixed(const core::Num& x, int places) { return fixed(x.value(), places); }
// format(x, ".N%"): x × 100 as fixed(…, places), then "%".
std::string percent(double x, int places);

}  // namespace genko::service
