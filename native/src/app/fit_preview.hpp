#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

// A picture of a whole page, kept whole (SPEC UX-02, I08): from the first time it is shown it is scaled to fit the
// part of the widget in sight with its proportions kept and centred, and it is fitted again whenever the widget's size
// changes or the body it is in is scrolled (the export dialog of M4 shows its preview with it). In a scrolling body
// the part in sight is where the body's viewport shows it (all of it when none of it is in sight). shown_rect() is
// where the page is drawn now.

namespace genko::app {

class FitPreview : public QWidget {
    Q_OBJECT

public:
    explicit FitPreview(QWidget* parent = nullptr);

    void set_image(const QImage& image);
    const QImage& image() const { return image_; }
    // Where the picture is drawn in the widget now (empty without a picture).
    QRectF shown_rect() const;
    // The part of the widget in sight in the scrolling body it is in (the whole widget when it is in none, or when none
    // of it is in sight).
    QRect in_sight() const;
    // The same for any size of widget (with the margin around the picture).
    static QRectF fit(const QSizeF& picture, const QSizeF& room, double margin = 6.0);

    QSize sizeHint() const override { return QSize(320, 420); }

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QImage image_;
    bool watching_ = false;
};

}  // namespace genko::app
