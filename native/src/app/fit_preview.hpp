#pragma once

#include <QImage>
#include <QRectF>
#include <QWidget>

// A picture of a whole page, kept whole (SPEC UX-02, I08): from the first time it is shown it is scaled to fit the
// widget with its proportions kept and centred, and it is fitted again whenever the widget's size changes (the export
// dialog of M4 shows its preview with it). shown_rect() is where the page is drawn now.

namespace genko::app {

class FitPreview : public QWidget {
    Q_OBJECT

public:
    explicit FitPreview(QWidget* parent = nullptr);

    void set_image(const QImage& image);
    const QImage& image() const { return image_; }
    // Where the picture is drawn in the widget now (empty without a picture).
    QRectF shown_rect() const;
    // The same for any size of widget (with the margin around the picture).
    static QRectF fit(const QSizeF& picture, const QSizeF& room, double margin = 6.0);

    QSize sizeHint() const override { return QSize(320, 420); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
};

}  // namespace genko::app
