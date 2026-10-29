#pragma once

#include <QRectF>
#include <QStringList>
#include <QWidget>

#include <cstdint>
#include <vector>

// The middle of the window (PC5 H-c, 節 H 5.3, the user's decision 7: ウィンドウ → アレンジ in one window): the page canvas
// alone — the tabs' way, as before —, or cells side by side (n-up, tiles), PhotoCraft's arrangements (view_cmds.rs cells,
// MIT). The front cell holds the window's one canvas (its tools, its panels follow it); the others a PaneView each (a
// tab's page, only looked at). The cells stay where they are: a pane clicked takes the canvas, the cell the canvas left
// takes the pane. Their sizes move with the dividers between them. At most kMostPanes cells (節 H 4.5, 3 章: the books
// past them are chosen by their tabs).

class QSplitter;
class QVBoxLayout;

namespace genko::app {

class PageCanvas;
class PaneView;

class DocArea : public QWidget {
    Q_OBJECT

public:
    static constexpr int kMostPanes = 16;

    explicit DocArea(PageCanvas* front, QWidget* parent = nullptr);

    // The arrangements of cells (window.arrange.*), in Photoshop's order: tileAllVertically, tileAllHorizontally,
    // twoUpVertical, twoUpHorizontal, threeUpVertical, threeUpHorizontal, threeUpStacked, fourUp, sixUp, tile. "tabs": the
    // canvas alone.
    static const QStringList& layouts();
    // How many cells an arrangement has for so many books (at most kMostPanes; 0: none — the tabs, or fewer than 2 books).
    static int capacity(const QString& layout, int books);
    // Where each cell lies in the middle of the window (a 1 × 1 square, row by row): PhotoCraft's cells.
    static std::vector<QRectF> cells(const QString& layout, int books);

    const QString& layout() const { return layout_; }
    // The cells laid out again: `tabs` the tab shown in each (the window's ids; as many as capacity, or fewer: the
    // rest of the cells empty), the canvas in cell `front`. "tabs" (or no tabs): the canvas alone.
    void arrange(const QString& layout, const std::vector<std::uint64_t>& tabs, int front);
    // The tab in each cell (none in the tabs' way), the cell of the canvas (-1 in the tabs' way), a tab's cell (-1).
    const std::vector<std::uint64_t>& tabs() const { return tabs_; }
    int front() const { return front_; }
    int cell_of(std::uint64_t tab) const;
    // The canvas to cell `to`; the pane that was there to the cell the canvas left (still showing its old tab: the window
    // gives it the one that left the front). The pane moved, or null.
    PaneView* bring_front(int to);
    // Another tab in a cell (the cell in front: the canvas shows it — the window's own business).
    void set_tab(int cell, std::uint64_t tab);
    PaneView* pane(int cell) const;
    std::vector<PaneView*> panes() const;
    // A cell's widget (its frame) — tests, and the window's look.
    QWidget* cell(int index) const;

signals:
    // A pane clicked (its cell).
    void paneActivated(int cell);

private:
    void clear();
    void mark();
    QWidget* take(int cell);
    void put(int cell, QWidget* content);

    PageCanvas* canvas_;
    QVBoxLayout* root_;
    QWidget* tree_ = nullptr;          // (the splitters, the cells in them)
    std::vector<QWidget*> cells_;      // (each cell's frame)
    std::vector<QWidget*> contents_;   // (what each holds: the canvas or a pane)
    std::vector<std::uint64_t> tabs_;
    std::vector<PaneView*> spare_;     // (panes kept from the last arrangement, given again)
    QString layout_ = QStringLiteral("tabs");
    int front_ = -1;
    QSize canvas_least_;               // (the canvas's own least size, given back in the tabs' way)
};

}  // namespace genko::app
