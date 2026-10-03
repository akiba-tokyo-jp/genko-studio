#pragma once

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

// Several books open at once, and one book in several windows (Python's genko/app/documents.py, J11): a window holds
// its books as tabs (each tab one Session: the book, its page, how it was shown); another window on the same book
// shares that Session, so a change made in one shows at once in the other, while each keeps its own page, zoom, turn
// and panels.

namespace genko::app {

class MainWindow;
class Session;

namespace documents {

void add(MainWindow* window);
void remove(MainWindow* window);
// The windows open now.
std::vector<MainWindow*> windows();
// (window, tab) where this book is open already, or (nullptr, -1).
std::pair<MainWindow*, int> find(const std::filesystem::path& path);
// Whether another window (in any of its tabs) still has this book open: closing it here must not close the book.
bool open_elsewhere(const Session* session, const MainWindow* but);

}  // namespace documents

}  // namespace genko::app
