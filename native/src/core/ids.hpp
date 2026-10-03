#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

// A new transaction id for the journal (schema-v4 §4.2): 32 lowercase hex digits, always random.
std::string new_txn_id();

// 32 lowercase hex digits (a transaction id or a book id).
inline bool is_txn_id(std::string_view text) { return is_book_id(text); }

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

// The ids new_id() gives on this thread while it lives: first `given` (in order), then new ones as usual; every id it
// gives is recorded (taken()). The app uses it to give a pen line the id its live picture was drawn with (the id is
// the seed of the brush's grain and scatter), and to apply a change again on a rebase with the ids it had. Scripts on
// one thread nest (the innermost one answers); other threads are not affected.
class ScopedIdScript {
public:
    explicit ScopedIdScript(std::vector<std::string> given = {});
    ~ScopedIdScript();
    ScopedIdScript(const ScopedIdScript&) = delete;
    ScopedIdScript& operator=(const ScopedIdScript&) = delete;

    // The ids new_id() gave while this script was the innermost one, in order.
    const std::vector<std::string>& taken() const { return taken_; }

private:
    friend std::string new_id();
    std::string next();

    std::vector<std::string> given_;
    std::size_t used_ = 0;
    std::vector<std::string> taken_;
    ScopedIdScript* outer_ = nullptr;
};

}  // namespace genko::core
