#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QListWidget;
class QListWidgetItem;
class QTimer;

// フキダシ (fukidashi@1, SPEC BALLOON-01 — the user's 「今風の漫画の吹き出しを各種作って置けるように」): the balloons a manga of
// now uses, each a preset — a kind, the style keys of its look and, for the ones that are about their tails, its
// tails — shown as a small picture the renderer itself draws (render::text::draw_group: what is picked is what is
// drawn). A click gives the chosen line that look, or, with the text tool in hand or no line chosen, makes it the look
// of the next line placed on the page (the window: main_window_fukidashi.cpp).
//
// The pictures are drawn once a process (kept by the preset and the screen's scale), the first time the panel is
// shown, a few at a time between the screen's events: opening the window draws none.

namespace genko::app {

struct BalloonPreset {
    const char* key;      // its name among the presets (kept: the tests', and what the window remembers)
    const char* name;     // its short name under its picture
    const char* tip;      // the voice it is for
    const char* kind;     // the line's balloon
    core::Json style;     // the keys of its look
    // Its tails: [{to: [fx, fy] (the tip, from the balloon's middle, in shares of its width and height), kind?}]
    core::Json tails;
    bool about_tails;     // (a line given the look takes these tails too: 内向きのしっぽ, しっぽ 2 本, 心の声の泡)
    bool across;          // (placed, its words run across: 横長)
};

// The presets, in the panel's order; one by its key (none: nullptr).
const std::vector<BalloonPreset>& balloon_presets();
const BalloonPreset* balloon_preset(const QString& key);

// The style keys that are a balloon's look: a line given a preset has each of these the preset does not say taken
// away (so 叫び（長短） turned ふつう leaves no spike behind), and nothing else of its style touched — its face, size,
// the letters' colour (but the white of ベタ白抜き, which goes with the black it was on), where its letters sit, its
// turn, the balloon eraser's cuts, the balloons it is joined with.
const std::vector<std::string>& balloon_look_keys();
// A preset's tails for a balloon's box (mm, the page's).
core::Json preset_tails(const BalloonPreset& preset, double x_mm, double y_mm, double w_mm, double h_mm);
// The one edit_line that gives a line the preset's look: its kind, its look's keys (the others of the look null), and
// — a preset about its tails — its tails (a line that has tails keeps where they point, each taking the preset's
// kind; one that has none, not as many as しっぽ 2 本 says, or tails pointing the other way — in or out — takes the
// preset's own). The line's words, place and size are not in it.
core::Json preset_restyle_op(const BalloonPreset& preset, const core::StoryLine& line);
// A preset's picture: a balloon of its look drawn by the renderer on white paper, `scale` device pixels a pixel.
QImage preset_picture(const BalloonPreset& preset, double scale = 1.0);
// The pictures' size (pixels of the screen, before the scale).
QSize preset_picture_size();

class BalloonPalette : public QWidget {
    Q_OBJECT
public:
    explicit BalloonPalette(QWidget* parent = nullptr);
    QListWidget* list() const { return list_; }
    // The preset marked as the one the next line takes (empty: none): its item in heavier letters on the accent's soft
    // colour, and the item in hand; no item is left chosen.
    void mark(const QString& key);
    QString marked() const;
    // Every picture drawn now (the tests; shown, they are drawn a few at a time).
    void draw_pictures();
    bool pictures_drawn() const { return next_ >= count_; }
    // A preset's item (none: nullptr).
    QListWidgetItem* item(const QString& key) const;

signals:
    // A preset picked (a click, Enter or Space on it). `how`: "" as the window decides (the chosen line, or the next
    // line placed), "line" the chosen line, "next" the next line placed — the item's own menu.
    void picked(const QString& key, const QString& how);

protected:
    void changeEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void draw_some(int most);

    QListWidget* list_ = nullptr;
    QTimer* timer_ = nullptr;
    int next_ = 0;
    int count_ = 0;
};

}  // namespace genko::app
