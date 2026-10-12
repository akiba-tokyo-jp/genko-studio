#pragma once

#include <QDialog>
#include <QGraphicsView>
#include <QImage>
#include <QString>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

class QLabel;

// A large, zoomable viewer for looking closely before deciding (Python's genko/app/viewer.py): pages, panels,
// candidates side by side. Opens fitted to the window; the wheel zooms, dragging moves, 0 fits again, 1 shows 100%.
// With several images they are laid out side by side with their captions; with a page list the arrows (or ← →) move
// between pages.

namespace genko::app {

using ViewerItem = std::pair<QString, QImage>;

class ImageView : public QGraphicsView {
    Q_OBJECT

public:
    ImageView();
    void show_items(const std::vector<ViewerItem>& items);
    void fit();
    void actual();
    bool fitted() const { return fitted_; }

protected:
    void wheelEvent(QWheelEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    bool fitted_ = true;
};

class ViewerDialog : public QDialog {
    Q_OBJECT

public:
    // items: what to show now. pages and load_page: an optional page list and a loader for ← →.
    ViewerDialog(QWidget* parent, const QString& title, const std::vector<ViewerItem>& items, const QString& note = {},
                 std::vector<int> pages = {}, std::function<std::vector<ViewerItem>(int)> load_page = {}, std::optional<int> current = {});

    void step(int delta);
    ImageView* view() const { return view_; }
    int page() const { return pages_.empty() ? 0 : pages_[static_cast<std::size_t>(index_)]; }
    QLabel* page_label() const { return page_label_; }

private:
    void label();

    std::vector<int> pages_;
    std::function<std::vector<ViewerItem>(int)> load_page_;
    int index_ = 0;
    ImageView* view_ = nullptr;
    QLabel* page_label_ = nullptr;
};

}  // namespace genko::app
