#pragma once

#include <QColor>
#include <QString>
#include <QVariant>

class QApplication;
class QWidget;
class QLabel;
class QAbstractButton;

// The screen's look (Python's genko/app/theme.py): one set of colours in two lights — calm neutral greys with a
// single accent — used by the palette, one style sheet and the icons, so every panel looks of a piece. The settings
// (環境設定, M6): ui/theme (system | light | dark), ui/brightness (-2 … +2), ui/surround (auto | 0…255), ui/mono_icons,
// ui/hints, ui/ui_font (genko: the bundled IBM Plex Sans JP | system).

namespace genko::app::theme {

struct Tokens {
    bool dark = false;
    QString window, panel, base, raised, hover, border, divider, text, muted, faint, accent, accent_text, accent_soft,
        selected, danger, ok, surround;
};

Tokens light();
Tokens dark();

// The colours now: the light or dark set, its greys moved by the brightness, the surround as chosen.
Tokens tokens();

bool show_hints();
void set_hints(bool on);

// The accent and the canvas's surround, for drawing.
QColor accent();
QColor surround();

// One style sheet for every panel (pt: the base size of the letters).
QString style_sheet(const Tokens& t, double pt);

// Put the look on the whole application (Fusion, the palette, the style sheet, the letters, Qt's own words in
// Japanese).
void apply(QApplication* app);

// A label's (or widget's) part in the look: hint, empty, error, title, heading, section, label, caption, badge…
QWidget* role(QWidget* widget, const char* name);
// An explanation in a panel: quiet, and hidden (its words kept as the parent's tooltip) when hints are off.
QLabel* hint(QLabel* label);
// The main button of a panel or dialog, in the accent.
QAbstractButton* primary(QAbstractButton* button);
void role_prop(QWidget* widget, const char* key, const QVariant& value);
// Picture-only buttons get their tooltip's first line as their name for screen readers.
void name_buttons(QWidget* root);

}  // namespace genko::app::theme
