#pragma once

// `genko app [book]`, and `genko` alone: the desktop app (Python's genko.app.main.run_app). Without a book the start
// screen comes first (a new book, the recent ones, or open one); with one, the window opens on it (read on a worker
// thread; a recovery copy newer than the book is offered first).

namespace genko::app {

// argv[1] is "app" (or there are no arguments). Returns the exit code.
int run_app(int argc, char** argv);

}  // namespace genko::app
