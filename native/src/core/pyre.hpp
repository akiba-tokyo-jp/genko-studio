#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// Python's re for what the ops ask of it (bookops.replace_text without "regex"): a pattern that is a text found as it
// is (re.compile(re.escape(find), flags) for a str), matched with or without re.IGNORECASE as CPython 3.12's _sre
// matches it, and pattern.subn(repl, text) with Python's replacement templates (\g<0>, \n, \t, octal escapes, …) and
// their errors. Patterns written in re's own syntax are not here.

namespace genko::core {

class PyLiteralPattern {
public:
    // re.compile(re.escape(find), re.IGNORECASE if ignore_case else 0)
    PyLiteralPattern(std::string_view find, bool ignore_case);

    // pattern.subn(repl, text): the text with every match (left to right, none overlapping) replaced by the template,
    // and how many matches there were. As subn does, the template is read before the text is searched, so a template
    // Python cannot read fails whatever the text: re.error (PyUncaught "re.error": "invalid group reference 1 at
    // position 1", "bad escape \q at position 0", …) or IndexError ("unknown group name 'x'").
    std::pair<std::string, std::int64_t> subn(std::string_view repl, std::string_view text) const;

private:
    std::u32string find_;
    bool ignore_case_;
};

}  // namespace genko::core
