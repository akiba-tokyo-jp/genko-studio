#pragma once

#include <QString>

#include <optional>
#include <string>

#include "core/json.hpp"
#include "core/model.hpp"

// 環境設定 → ヒストリーログ (PC5 H-e, edit.preferences.historyLog; PhotoCraft v0.2.0's prefs.rs HistoryLog and prefs_ui.rs
// history_log, MIT; SPEC VIEW-01): the edits written down on this computer, never in the book.
// - テキストファイル: a line for each change of a book open in the app (the time, the book, the change in words; 詳細 its ops'
//   names too), and when a book is opened and closed (セッションのみ: those alone), appended to the file chosen. The file
//   is never a book, inside one, a file a smart object links nor a picture a book was opened from (formats::write_guard,
//   as the exports' files): refused, said once.
// - メタデータ: the files written from a book (the exports, コピーを保存…, 書き出し形式…) carry its edits from its journal as
//   Photoshop's photoshop:History (formats::set_history_source; the latest 1000 changes).

namespace genko::app {

class Session;

namespace history_log {

// Its changes written (once for a book however many windows show it). The first window starts it.
void follow(Session* session);
// The metadata's source given to the exports (once, at the first window).
void install();
// A line of the text file (tests): the time, the book's title, the words (and the ops' names with 詳細).
QString line(const QString& title, const QString& words, const core::Json& ops, bool detailed);
// What the metadata holds for a book now: none while the log is off or not kept in the metadata.
std::optional<std::string> metadata(const core::Document& doc);

}  // namespace history_log
}  // namespace genko::app
