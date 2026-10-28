#pragma once

#include <functional>

// settings.ini watched (PC5 H-0, 節 H 2.3; SPEC VIEW-01): a preference written by another program while the app is open
// — `genko prefs set` (H-e), another Genko — is applied to the open windows. Only the table's preferences that show at
// once count (core::prefs::Apply::Live, the keys too): the tools' memories the app writes all the time never call it.
// The app's own writes may call it too (it then applies what it has already applied: nothing changes).

namespace genko::app {

// Watch from now (once for the app: a later call only changes what is called). `changed` is called on the app's thread,
// a moment after the file stops changing, when a preference's value is not what it was.
void watch_settings(std::function<void()> changed);
// The preferences' values looked at again now (a test: what the watch compares with; the call made if they changed).
bool settings_changed_now();

}  // namespace genko::app
