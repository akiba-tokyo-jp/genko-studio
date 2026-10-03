#pragma once

#include <QObject>

#include <cstdint>
#include <initializer_list>
#include <utility>

#include "core/json.hpp"

class QInputEvent;

// Timings for the performance runs (docs/cpp-migration/ACCEPTANCE.md AC-PERF; the internal times of §6, not a pen's
// physical latency): with GENKO_PERF_LOG=<file>, one JSON object per line — {"ev": <what>, "t": <monotonic ns>, …} —
// for the input events (with Qt's event timestamp), the live line drawn, the screen update asked for, the paint done,
// a line committed, an undo, a page switch (first picture and the fine one), a save started and done, and the event
// loop's stalls. Without the variable nothing is recorded and nothing changes; with it the app behaves the same (the
// lines are written by a timer, a second at a time).

namespace genko::app::perf {

bool enabled();
// Monotonic nanoseconds (the clock of "t").
std::int64_t now_ns();
void event(const char* name, std::initializer_list<std::pair<const char*, core::Json>> fields = {});
void event(const char* name, core::Json fields);
// An input event: its kind, Qt's timestamp (ms) and a sequence number for the live line it leads to.
std::uint64_t input(const QInputEvent* event, const char* kind);
// Start watching the event loop (a 5 ms timer: a tick late by more than 4 ms is a stall, recorded).
void watch_loop(QObject* parent);
void flush();

}  // namespace genko::app::perf
