#pragma once

#include <QIcon>
#include <QListWidget>
#include <QString>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/thumbs.hpp"
#include "core/model.hpp"

// The page list (Python's genko/app/pages_panel.py): a small picture of every page, drag to reorder, and a menu for
// adding, copying, moving and deleting, spreads and the page's nombre. The pictures come from the lasting cache and are
// made only for the rows in sight, one at a time on a worker thread, so the window stays quick however long the book is.

namespace genko::app {

// A page's label in the list (Python's MainWindow._page_text): its number, the name and art stages, a spread, a
// hidden nombre, who draws it; a cover's kind.
QString page_text(const core::Page& page);

class PageList : public QListWidget {
    Q_OBJECT

public:
    static constexpr int kThumbHeight = 100;

    explicit PageList(QWidget* parent = nullptr);

    // The list from the book's pages (the pictures of unchanged pages kept), `current` chosen.
    void fill(DocPtr doc, int current);
    // Ask for the pictures of the rows in sight that have none (or an old one).
    void request_visible();
    // Wait (processing events) until the rows in sight have their pictures (tests).
    bool wait_pictures(int ms);
    ThumbMaker& maker() { return *maker_; }
    // The pages whose pictures are shown (a picture each), and whether a row has its picture.
    bool has_picture(int row) const;

signals:
    void reorderRequested(const std::vector<int>& order, int moving);
    void addAfterRequested(int index);
    void duplicateRequested(int index);
    void deleteRequested(int index);
    // the menu's spreads (MainWindow.set_spread): with the page `other`, or undone; the page's nombre shown or hidden
    void spreadRequested(int index, int other);
    void spreadUndone(int index);
    void numeroRequested(int index, bool numero);

protected:
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void show_menu(const QPoint& pos);
    QIcon blank() const;

    DocPtr doc_;
    std::unique_ptr<ThumbMaker> maker_;
    std::map<std::string, std::pair<const core::Page*, QIcon>> pictures_;  // page id → (the page it shows, picture)
    std::map<std::string, const core::Page*> asked_;  // requested and not back yet
};

}  // namespace genko::app
