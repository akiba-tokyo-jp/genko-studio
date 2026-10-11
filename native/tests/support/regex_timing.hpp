#pragma once

// How long a search refused when its steps are spent (core/pyre, SPEC COMP-01a) may take, whatever the build: what a
// test of those refusals means is that the search stops in proportion to the steps it was given — no work left
// uncounted that would let it run on — not that it takes a release build's second. So the bound is measured here and
// now: the time this build takes for a known number of steps of a search whose every step is one pass of sre's loop,
// scaled to the steps given, with a margin. (A sanitizer build at -O0 runs sre's loop some 90 times slower than a
// release one: 62 s for the 100 million steps a release build spends in 0.7 s.) An optimized build without sanitizers
// also keeps the test's absolute bound.

#include <QElapsedTimer>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "core/error.hpp"
#include "core/pyre.hpp"

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define GENKO_TEST_SANITIZED 1
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(undefined_behavior_sanitizer)
#ifndef GENKO_TEST_SANITIZED
#define GENKO_TEST_SANITIZED 1
#endif
#endif
#endif

namespace genko::test {

// An optimized build without sanitizers (NDEBUG; no AddressSanitizer, ThreadSanitizer, MemorySanitizer, or
// UndefinedBehaviorSanitizer where the compiler says so): the one the absolute bounds were set for.
#if defined(NDEBUG) && !defined(GENKO_TEST_SANITIZED)
inline constexpr bool kOptimizedBuild = true;
#else
inline constexpr bool kOptimizedBuild = false;
#endif

// The steps measured, and the margin over the steps given at the measured rate.
inline constexpr std::uint64_t kCalibrationSteps = 1'000'000;
inline constexpr std::int64_t kStepMargin = 3;

// Nanoseconds this build takes, now, for kCalibrationSteps of sre's steps: (a|aa)+$ over 60 a's and "!", which
// backtracks without end, each step one pass of sre's loop (a branch, a mark, a repeat) with nothing between steps but
// one group's two marks — the steps of the refusals' own searches, and the dearest kind per step (a step of the work
// between sre's steps, a mark copied or an item of a set looked through, is a few instructions). The best of three
// measures: the machine's own interruptions taken out.
inline std::int64_t regex_step_cost() {
    const std::string text = std::string(60, 'a') + "!";
    const core::PyPattern pattern("(a|aa)+$", 0);
    std::int64_t best = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < 3; ++i) {
        core::PyReBudget budget{kCalibrationSteps};
        QElapsedTimer clock;
        clock.start();
        try {
            (void)pattern.subn("x", text, nullptr, budget);
        } catch (const core::Error&) {  // (regex_too_slow: what is measured)
        }
        best = std::min<std::int64_t>(best, clock.nsecsElapsed());
    }
    return best;
}

// The longest searches given `steps` steps in all may take (ms): those steps at the measured rate, times kStepMargin —
// the long search on a machine busier than during the short measure (the tests run two at a time), its caches, the
// refusal's own end — and never under 250 ms (a measure of a few milliseconds is mostly the clock's). A release build:
// 7 ms measured, so about 2.1 s for 100 million steps (spent in 0.7 s); a sanitizer build at -O0: about 186 s (62 s).
inline std::int64_t regex_steps_bound_ms(std::int64_t calibration_ns, std::uint64_t steps) {
    const double per_step_ns = static_cast<double>(calibration_ns) / static_cast<double>(kCalibrationSteps);
    return std::max<std::int64_t>(250, static_cast<std::int64_t>(per_step_ns * static_cast<double>(steps) * static_cast<double>(kStepMargin) / 1e6));
}

}  // namespace genko::test
