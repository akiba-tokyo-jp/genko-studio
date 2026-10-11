#pragma once

#include <string>

// The time of day as the studio records it (Python's time.strftime("%Y-%m-%dT%H:%M:%S"), local time): when a person let
// approvals come through a chat (allow_chat_approval). Tests fix it (ScopedClock), as the Python reference harness
// fixes time.strftime, so the books compare exactly.

namespace genko::core {

// time.strftime("%Y-%m-%dT%H:%M:%S"): the local time now, to the second ("2026-10-11T09:30:00").
std::string local_time_text();

// Tests only: while it lives, local_time_text() returns `text` (one at a time; not for threads).
class ScopedClock {
public:
    explicit ScopedClock(std::string text);
    ~ScopedClock();
    ScopedClock(const ScopedClock&) = delete;
    ScopedClock& operator=(const ScopedClock&) = delete;

private:
    std::string previous_;
    bool had_previous_ = false;
};

}  // namespace genko::core
