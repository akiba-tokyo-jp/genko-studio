#pragma once

#include <QString>

#include <functional>
#include <utility>

// settings.ini watched (PC5 H-0, 節 H 2.3; SPEC VIEW-01): a preference written by another program while the app is open
// — `genko prefs set` (H-e), another Genko — is applied to the open windows. Only the table's preferences that show at
// once count (core::prefs::Apply::Live, the keys too): the tools' memories never call it. The app's own writes (through
// settings(): app/config) never call it either — what the app writes it has applied itself —: the watch takes its base
// again after them. Only settings.ini itself is looked at (the config folder's other files leave it be).

namespace genko::app {

// settings.ini as it stands: (modified, ms since the epoch; size), or (0, 0) when it is not there.
using SettingsStamp = std::pair<long long, long long>;
SettingsStamp settings_stamp(const QString& file);
// The app wrote settings.ini, leaving it so (app/config's settings() when its store goes and the file changed).
void settings_written(const SettingsStamp& stamp);

// Watch from now (once for the app: a later call only changes what is called). `changed` is called on the app's thread,
// a moment after the file stops changing, when a preference's value is not what it was.
void watch_settings(std::function<void()> changed);
// The preferences' values looked at again now (a test: what the watch compares with; the call made if they changed).
bool settings_changed_now();

}  // namespace genko::app
