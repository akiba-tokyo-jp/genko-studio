#pragma once

#include <QImage>
#include <QWidget>

#include <array>
#include <optional>

class QComboBox;

// サブビュー (Python's genko/app/subview.py): reference pictures beside the page — a photo, a character sheet, a colour
// scheme — to look at while drawing and to take colours from (click: the pen's colour). The list is kept in the
// settings (subview/images, the paths split by tabs, as Python keeps them), so it is there for every book.

namespace genko::app {

class MainWindow;

// The pictures kept (the ones still there), and keeping them.
QStringList kept_pictures();
void keep_pictures(const QStringList& paths);

// The picture, fitted to the panel; a click reports the colour under it.
class SubPicture : public QWidget {
    Q_OBJECT
public:
    SubPicture();
    void set_image(const QImage& image);
    const QImage& image() const { return image_; }
    // The colour under a point of the panel (none: outside the picture).
    std::optional<std::array<int, 3>> colour_at(const QPointF& pos) const;

signals:
    void picked(const std::array<int, 3>& rgb);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    struct Target {
        double x, y, scale;
    };
    std::optional<Target> target() const;
    QImage image_;
};

class SubView : public QWidget {
    Q_OBJECT
public:
    explicit SubView(MainWindow* window);
    // The list read again and the chosen picture shown.
    void refresh();
    void show_current();
    // A picture added (and shown); false: it cannot be opened.
    bool add(const QString& path);
    void add_dialog();
    void remove_current();

    QComboBox* choice = nullptr;
    SubPicture* picture = nullptr;

protected:
    // (the picture is read when the panel is first shown, not with the window: a big photo costs nothing until then)
    void showEvent(QShowEvent* event) override;

private:
    void fill();
    MainWindow* window_ = nullptr;
    bool shown_ = false;  // a picture has been read (show_current)
};

}  // namespace genko::app
