#pragma once

#include <QIcon>
#include <QString>

// Tool and command icons (Python's genko/app/icons.py): Lucide (ISC licence; the SVGs of src/genko/app/lucide/ are
// built into the app as Qt resources) for everything a general icon set has, and a few manga pictures Genko draws
// itself in the same line weight. All are drawn in the look's text colour; a chosen tool's picture (a checked button)
// turns the accent.

namespace genko::app::icons {

// The picture in the look's colours now.
QIcon icon(const char* name);
// The picture in these colours (#rrggbb): `ink` for its lines, `accent` when its button is checked.
QIcon icon(const char* name, const QString& ink, const QString& accent);

// Load the resources of the app (icons, templates, letters) when the app is in a static library.
void init_resources();

}  // namespace genko::app::icons
