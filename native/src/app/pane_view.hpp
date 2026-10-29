#pragma once

#include <QTimer>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "app/canvas.hpp"

// A pane beside the one in front (PC5 H-c, 節 H 5.3, the user's decision 7: ウィンドウ → アレンジ's n-up and tiles): the page
// of another tab of the window, only looked at — a PageCanvas made view-only. It takes no tool: a click (the mouse or the
// pen) brings its tab to the front, into this cell (the window moves its canvas here); the wheel scrolls and zooms it
// (Ctrl), the middle button and Space+drag (Space held here, or on the canvas in front) drag it (節 H 5.3). It keeps the
// whole page's tiles only, drawn on the panes' one pool (DocArea's), and draws its book's changes again once they stop
// coming (節 H 4.5: never while the person works in the pane in front).

namespace genko::app {

class Session;

class PaneView : public PageCanvas {
    Q_OBJECT

public:
    // What the pane shows of its tab now: the book (a smart object's contents' tab: its contents alone), the page and how
    // it is drawn (proof | name).
    struct Shown {
        DocPtr doc;
        std::size_t index = 0;
        std::string mode = "proof";
    };
    using Source = std::function<std::optional<Shown>()>;

    explicit PaneView(QWidget* parent = nullptr);

    // Show a tab: its id (the window's), its book (its changes followed), what to draw, and its view (none: fitted).
    void show_tab(std::uint64_t tab, std::shared_ptr<Session> session, Source source, const std::optional<ViewState>& view);
    std::uint64_t tab() const { return tab_; }
    // Draw it again now from its source (the book as it is now).
    void refresh();
    // How long after its book's last change it draws it again (ms).
    static constexpr int kLaterMs = 500;
    bool waiting() const { return later_.isActive(); }
    // Whether Space is held on the canvas in front (the keys go there: Space+drag here pans this pane too).
    std::function<bool()> space_down;

signals:
    // A click on it: its tab to the front, here.
    void activated();

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void tabletEvent(QTabletEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    std::uint64_t tab_ = 0;
    std::shared_ptr<Session> session_;
    Source source_;
    QMetaObject::Connection follows_;
    QTimer later_;
    bool middle_ = false;  // (dragging its view: the middle button, or Space with the left)
    bool space_ = false;   // (Space held with the focus here)
};

}  // namespace genko::app
