#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QImage>
#include <QSet>
#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QTimer;

// 書体 (fonts@1, 書体の追加 F2 — the user's 「書体も足りない気がするのですよね」): every lettering face Genko comes with — the seven
// of before and the forty added —, under its category, each with a sample the renderer itself sets (render::text::
// text_layout: the face's kana, its partner's kanji and its own ！ as a page would show them) and a mark for what a
// person must know before choosing it (かなだけ, 漢字は一部, ファイルなし). A box above narrows the list by a face's name,
// its use or its category.
//
// A click gives the chosen line the face, or — the text tool in hand, or no line chosen — makes it the face of the next
// line placed; the item's own menu also sets a chosen ruby's face and the book's usual face (the window:
// main_window_fonts.cpp).
//
// Nothing is read or drawn until the panel is first shown; then the samples come one at a time between the screen's
// events, and an added face's file (megabytes, read whole and checked against the catalogue) is read ahead of its sample
// on a worker thread — one file at a time, the next not before this one's sample is drawn, its bytes held until then —,
// so the window is never held: the screen's thread only sets five letters from bytes that are already there. A face whose file is not on this
// computer is greyed and says so; it cannot be picked.

namespace genko::app {

// A face's sample: 「あア漢字！」 set across in the face, black on nothing (the list tints it with its own text colour),
// `scale` device pixels a pixel. Kept by the face and the scale. An empty picture for a face that is not installed.
QImage face_sample(const QString& key, double scale = 1.0);
// The samples' size (pixels of the screen, before the scale).
QSize face_sample_size();

class FontPalette : public QWidget {
    Q_OBJECT
public:
    explicit FontPalette(QWidget* parent = nullptr);
    QListWidget* list() const { return list_; }
    QLineEdit* search() const { return search_; }
    // A face's row (none: nullptr), and a category's heading.
    QListWidgetItem* item(const QString& key) const;
    QListWidgetItem* heading(const QString& category) const;
    // The rows shown now (not hidden by the search), faces only.
    QStringList shown() const;
    // Whether a face's row says its file is not on this computer (greyed, not to be picked).
    bool missing(const QString& key) const;
    // The files looked for again (one stat a face: when the panel is shown, and when asked).
    void refresh_files();
    // The book's usual face (empty: none of its own — アンチック): its row says 「この本のいつもの書体」.
    void set_usual(const QString& key);
    QString usual() const;
    // The face marked as the one the next line takes (empty: none).
    void mark(const QString& key);
    QString marked() const;
    // Every sample drawn now (the tests; shown, they are drawn one at a time).
    void draw_samples();
    bool samples_drawn() const { return next_ >= static_cast<int>(order_.size()); }
    int samples_made() const { return next_; }
    // How many faces' files were read ahead of their samples off the screen's thread, and the longest the screen's
    // thread was held by one sample (milliseconds) — what the tests and a measurement look at.
    int files_read_ahead() const { return static_cast<int>(warmed_.size()); }
    int slowest_sample_ms() const { return slowest_ms_; }
    // A face's own menu (the right click, the menu key; not shown): 選んだ台詞をこの書体にする ("line"), 次に置く台詞を
    // この書体にする ("next"), 選んだルビをこの書体にする ("ruby"), この本のいつもの書体にする ("book").
    std::unique_ptr<QMenu> item_menu(const QString& key);

signals:
    // A face picked (a click, Enter or Space on it; its menu). `how`: "" as the window decides.
    void picked(const QString& key, const QString& how);

protected:
    bool event(QEvent* event) override;
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void draw_some(int most);
    void step();
    void show_sample(QListWidgetItem* item);
    void filter(const QString& words);

    QLineEdit* search_ = nullptr;
    QLabel* none_ = nullptr;
    QListWidget* list_ = nullptr;
    QTimer* timer_ = nullptr;
    std::vector<QListWidgetItem*> order_;  // (the faces' rows, in the list's order)
    int next_ = 0;
    using HeldBytes = std::vector<std::shared_ptr<const QByteArray>>;
    QFutureWatcher<HeldBytes>* warming_ = nullptr;  // (the worker reading the next face's file, and its partner's)
    HeldBytes held_;                                 // (what it read, held until the sample is set from it)
    QString warming_key_;
    QSet<QString> warmed_;  // (the faces whose files a worker has read)
    int slowest_ms_ = 0;
};

}  // namespace genko::app
