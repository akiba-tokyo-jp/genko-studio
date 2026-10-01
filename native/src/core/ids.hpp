#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace genko::core {

// A new id: 12 lowercase hex digits from the system's cryptographic random source (Python's uuid4().hex[:12]).
std::string new_id();

// A new page id: "pg_" + new_id().
std::string new_page_id();

// A new book id (project.json "book_id"): 32 lowercase hex digits, a version 4 UUID without its dashes. Always
// random, even inside a ScopedIdSource.
std::string new_book_id();

// 32 lowercase hex digits.
bool is_book_id(std::string_view text);

// Tests only: while it lives, new_id() returns ids from `source` instead of random ones (one at a time; not for
// threads). The Python reference harness has the same switch, so books whose ids are made while reading can be
// compared exactly.
class ScopedIdSource {
public:
    using Source = std::function<std::string()>;
    explicit ScopedIdSource(Source source);
    ~ScopedIdSource();
    ScopedIdSource(const ScopedIdSource&) = delete;
    ScopedIdSource& operator=(const ScopedIdSource&) = delete;

private:
    Source previous_;
};

// "000000000001", "000000000002", … (tools/migration/pyref_harness.py --ids makes the same).
ScopedIdSource::Source counting_ids(std::uint64_t first = 1);

}  // namespace genko::core
