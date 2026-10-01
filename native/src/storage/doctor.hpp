#pragma once

#include <filesystem>

#include "core/json.hpp"

// `genko doctor` (Python's maintenance.doctor): what is wrong with a book, without changing anything.
//   {"ok": no problems, "version", "revision", "problems": [text, …], …}
// Python's checks (each layer's picture and strokes asset, the length of the book's path) for every version; for v4
// books also the masks' and patches' pictures, the features this build does not know, what the reader had to
// report, the journal (a cut last line, lines that are not JSON, unfinished transactions and how the next write will
// settle them, a project.json that is not the state of the last commit, history states that are missing), and the
// audit (approval records missing or written twice). v1–v3 books get "needs_migration": true.

namespace genko::storage {

core::Json doctor(const std::filesystem::path& dir);

}  // namespace genko::storage
