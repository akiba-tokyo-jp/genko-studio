#pragma once

#include <QDialog>
#include <QPixmap>
#include <QTimer>
#include <QWidget>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/model.hpp"

class QSlider;

// 本の形でプレビュー (Python's genko/app/bookview.py): the book as a reader holds it — spreads, the cover first, a page
// turning over — read the way it is bound (right-bound books from right to left).

namespace genko::app {

class MainWindow;

namespace bookview {

constexpr int kPreviewDpi = 45;

// What the reader turns through: (kind, page) — the front, the pages, the back (a jacket gives both).
std::vector<std::pair<std::string, const core::Page*>> book_pages(const core::Document& doc);
// Which pages lie open together: the first alone (the cover, or page 1), then pairs.
std::vector<std::vector<int>> spreads(int count);
// Which of `pages` lie open together, as the book is bound: each cover alone, and the pages paired only where they
// face each other (the same pairs the pre-press check and the page sides use).
std::vector<std::vector<int>> book_spreads(const core::Document& doc, const std::vector<std::pair<std::string, const core::Page*>>& pages);

class BookPreview;

class BookView : public QWidget {
public:
    explicit BookView(BookPreview* dialog);
    // Where the open spread's pages are drawn (left, right; empty for none), each as big as its own paper (PC3 C-e: a
    // page of its own paper), at one scale.
    std::pair<QRectF, QRectF> page_rects();
    double turn = 0.0;  // 0..1 while a page turns
    int direction = 1;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    BookPreview* dialog_;
};

class BookPreview : public QDialog {
    Q_OBJECT

public:
    explicit BookPreview(MainWindow* window);

    // (left, right): the pages (in pages()) of an open spread.
    std::pair<std::optional<int>, std::optional<int>> sides(int spread) const;
    QString caption() const;
    std::optional<int> turning_page() const { return turning_; }
    const QPixmap& pixmap(int index);
    void go(int spread);
    void flip(int step);
    // One step of the turn (16 ms apart while it turns).
    void step();
    int spread() const { return spread_; }
    bool rtl() const { return rtl_; }
    BookView* view = nullptr;
    QSlider* slider = nullptr;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    std::shared_ptr<const core::Document> doc_;
    std::vector<std::pair<std::string, const core::Page*>> pages_;
    std::vector<std::vector<int>> groups_;
    bool rtl_ = true;
    int spread_ = 0;
    std::map<int, QPixmap> cache_;
    std::optional<int> turning_;
    int target_ = 0;
    QTimer timer_;
};

}  // namespace bookview
}  // namespace genko::app
