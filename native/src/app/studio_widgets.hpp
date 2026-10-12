#pragma once

#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QPointF>
#include <QWidget>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/review_model.hpp"
#include "core/json.hpp"

class QComboBox;
class QHBoxLayout;
class QListWidget;
class QPushButton;
class QTextEdit;

// The studio's panels for the person (Python's genko/app/studio_widgets.py): the process bar, the approval box, the
// panel view and the library. The widgets only read the window's book and send ops through it as the person (the
// window's session, human:<name>): nothing here approves on its own — every approval is a button a person presses on
// one request after looking at it.
//
// C++ (SPEC UX-02, improvement I13): the words for the AI — a reply, the reason a request is sent back, a panel's
// instruction — are written in several lines (ReplyEdit: Enter a new line, Ctrl+Enter sends), asked about once more
// before they go (who gets them and what), and kept as a draft for each book and request until they are sent.

namespace genko::app {

class MainWindow;
class ReplyEdit;

// A picture from the book's assets (asset_image): none without a book on disk, a ref or a picture there.
std::optional<QImage> asset_image(const std::optional<std::filesystem::path>& project, const std::string& ref);
// A picture made no larger than `box` (Image.thumbnail: it never grows).
QImage thumbnail(const QImage& image, QSize box);
// The shot and angle in words, then the action and the emotion (_brief_words).
QStringList brief_words(const core::Json& panel);

// The key of the window's book for the reply drafts (Drafts): its folder, or (not saved yet) "book:<id>". The book's
// drafts are placed first (Drafts::place): kept in its folder when it can be changed and is a v4 book.
QString draft_book_key(const MainWindow& window);

// A short note at the bottom of the window's page that goes away by itself (a click closes it: comfort.toast).
QLabel* toast(MainWindow* window, const QString& text, int ms = 5000);

// --- process bar ---------------------------------------------------------------------------------------------------

// How far the book is: only the steps that have pages, and the approval box's count (a click opens the box).
class ProcessBar : public QWidget {
    Q_OBJECT

public:
    ProcessBar();
    void refresh(const core::Document& doc);
    const std::vector<QLabel*>& labels() const { return labels_; }

signals:
    void openBox();

private:
    QHBoxLayout* layout_ = nullptr;
    std::vector<QLabel*> labels_;
};

// --- approval box ----------------------------------------------------------------------------------------------------

// A preview that keeps its picture fitted to the space it has; a double click opens the viewer.
class PreviewLabel : public QLabel {
    Q_OBJECT

public:
    PreviewLabel();
    void set_image(const std::optional<QPixmap>& pix);
    bool has_image() const { return source_.has_value(); }
    QWidget* corner = nullptr;  // (a button over the picture's top right corner)

signals:
    void activated();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void fit();
    std::optional<QPixmap> source_;
};

// Requests from the agent. Look at one, then approve it or send it back with a reason.
class ApprovalBox : public QWidget {
    Q_OBJECT

public:
    explicit ApprovalBox(MainWindow* window);

    void refresh();
    std::optional<review_model::InboxItem> current() const;
    const std::vector<review_model::InboxItem>& items() const { return items_; }
    void approve();
    void send_back();
    void open_viewer();
    // The draft's key of the book in front (its folder, or the book's id while it is not saved).
    QString book_key() const;

    // (tests and the window)
    QListWidget* list = nullptr;
    QLabel* detail = nullptr;
    QComboBox* page_pick = nullptr;
    PreviewLabel* preview = nullptr;
    QPushButton* big = nullptr;
    QListWidget* choices = nullptr;
    ReplyEdit* reason = nullptr;
    QPushButton* approve_button = nullptr;
    QPushButton* back_button = nullptr;
    QLabel* empty = nullptr;
    QLabel* count = nullptr;

signals:
    void changed();

private:
    QString kind_of(const review_model::InboxItem& item) const;
    void show_item(int row, bool follow = true);
    void page_changed();
    std::vector<std::pair<QString, core::Json>> numbered_sheets(const review_model::InboxItem& item) const;
    void sheet_choices(const review_model::InboxItem& item);
    const core::Page* page_of(const review_model::InboxItem& item) const;
    QImage page_image(const review_model::InboxItem& item, const core::Page& page, int dpi) const;
    void show_preview();
    void run_export();

    MainWindow* window_;
    std::vector<review_model::InboxItem> items_;
};

// --- panel view ------------------------------------------------------------------------------------------------------

// The panel picture; in region mode a drag draws a rectangle (in fractions of the picture).
class RegionImage : public QLabel {
    Q_OBJECT

public:
    RegionImage();
    bool drawing = false;
    std::vector<std::pair<QString, std::array<double, 4>>> boxes;  // (label, box in 0..1 of the picture)
    QRectF picture_rect() const;

signals:
    void drawn(double x, double y, double w, double h);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    std::optional<QPointF> start_;
    std::optional<QPointF> end_;
};

// One panel: the adopted art, the name over a candidate, the source image; the candidates with their provenance; the
// person's instruction; the regions.
class PanelView : public QWidget {
    Q_OBJECT

public:
    explicit PanelView(MainWindow* window);

    std::optional<std::string> frame_id;
    void set_frame(const std::optional<std::string>& id);
    void refresh();
    void adopt();
    void send_instruction();
    void delete_region();
    void open_viewer();
    void compare_candidates();

    // (tests)
    QComboBox* mode = nullptr;
    RegionImage* image = nullptr;
    QLabel* info = nullptr;
    QListWidget* candidates = nullptr;
    QTextEdit* provenance = nullptr;
    ReplyEdit* instruction = nullptr;
    QPushButton* send_button = nullptr;
    QComboBox* region_kind = nullptr;
    QPushButton* region_button = nullptr;
    QListWidget* regions = nullptr;

signals:
    void changed();

private:
    const core::Frame* frame() const;
    std::optional<core::Json> selected_candidate() const;
    void candidate_selected();
    void show_image();
    std::vector<std::pair<QString, std::array<double, 4>>> region_boxes(const core::Frame& frame) const;
    void region_mode(bool on);
    void region_drawn(double x, double y, double w, double h);

    MainWindow* window_;
};

// --- library ---------------------------------------------------------------------------------------------------------

// Characters (sheets, faces, lock) and locations (reference pictures), with where they came from.
class Library : public QWidget {
    Q_OBJECT

public:
    explicit Library(MainWindow* window);
    void refresh();

    QListWidget* list = nullptr;
    QTextEdit* detail = nullptr;
    const std::vector<core::Json>& entries() const { return entries_; }

private:
    void add(const QString& label, const std::optional<std::string>& asset, core::Json data);
    void show_entry();

    MainWindow* window_;
    std::vector<core::Json> entries_;
};

}  // namespace genko::app
