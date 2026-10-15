#pragma once

// The heap's free room given back to the system once the person has stopped for a while (ACCEPTANCE.md PERF-G: hours
// of work keep the memory flat). The C library keeps what was freed — by the drawing threads above all — for later use;
// when no input has come for a while, the room it keeps free goes back to the system (Linux: malloc_trim; Windows:
// _heapmin). Nothing the app holds is let go: no picture, no cache, no step of the history.

namespace genko::app::memory {

// The C library's heap set for a long session (glibc: at most two arenas, so the drawing threads share their room;
// blocks of 256 KiB and more — the tiles and pictures — from the system and back to it when freed). At the start of the
// program, before any thread (`genko app` does it first); later calls do nothing.
void tune();
// Watch the application's input (once; later calls do nothing): after 10 s without any, the free room goes back, and
// again every 5 s while none comes (the work that ends meanwhile leaves its room too).
void watch_idle();
// The free room back to the system now.
void give_back();

}  // namespace genko::app::memory
