#pragma once

#include <string>
#include <string_view>

namespace genko::core {

// Python's base64.b64encode(data).decode("ascii"): the standard alphabet with "=" padding, no line breaks.
std::string b64encode(std::string_view bytes);

// Python's binascii.a2b_base64(text) (not strict): characters outside the alphabet are skipped, decoding stops at
// a complete "=" padding, and a final group of the wrong length throws core::Error("format") with Python's
// message ("Incorrect padding", …). Text that is not ASCII throws too, as it does for a Python str.
std::string a2b_base64(std::string_view text);

}  // namespace genko::core
