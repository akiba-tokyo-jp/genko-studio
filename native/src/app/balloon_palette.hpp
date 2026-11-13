#pragma once

#include <QImage>
#include <QString>
#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

class QListWidget;
class QMenu;
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
// The style change that gives a line the preset's look (only the keys that change): every key of the look the line
// has and the preset does not say, null; the preset's own. Letters: ベタ's white only for a line whose letters are the
// usual black (none said, or dark) — a colour the person chose stays; white letters go back to the usual only when
// they went with a dark inside (白抜き文字 on a white or absent inside are left). A line of before whose wobble goes
// and that is an oval then is given hand: false, so its letters stay in the ellipse they were fitted in.
core::Json preset_style_change(const BalloonPreset& preset, const core::StoryLine& line);
// A line joined with the picked one (the same style.group on its page), and the box it is refitted to (null: kept).
struct JoinedLine {
    const core::StoryLine* line = nullptr;
    core::Json box;
};
// The ops that give a line the preset's look, applied together as one step: its edit_line — its kind, its look's keys
// and, for a preset about its tails, its tails (a line that has tails keeps where they point, each taking the
// preset's kind; one that has none, not as many as しっぽ 2 本 says, or any tail pointing the other way — in or out —
// takes the preset's own) —; `box` ({x_mm, y_mm, w_mm, h_mm}; null: kept), the move_line that refits it (the preset's
// tails said there, for that box); and for each line joined with it the kind and the look too (a joined balloon is
// drawn with one inside and one line). Nothing that would change nothing: an empty list when the line is that look
// already.
core::Json preset_restyle_ops(const BalloonPreset& preset, const core::StoryLine& line, const core::Json& box = core::Json(),
                              const std::vector<JoinedLine>& joined = {});
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
    // A preset's own menu (the right click, the menu key; not shown): 選んだ台詞のフキダシをこの形にする (picked with
    // "line") and 次に置く台詞をこの形にする ("next").
    std::unique_ptr<QMenu> item_menu(const QString& key);

signals:
    // A preset picked (a click, Enter or Space on it). `how`: "" as the window decides (the chosen line, or the next
    // line placed), "line" the chosen line, "next" the next line placed — the item's own menu.
    void picked(const QString& key, const QString& how);

protected:
    bool event(QEvent* event) override;
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
