#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <optional>
#include <vector>

// Writing Genko into an AI client's own settings (C++ only: SPEC AI-01, PLAN M5 — Python shows the text to paste and
// where, and the person pastes it). Only the two files Python's words name are written: Claude Desktop's
// claude_desktop_config.json (its mcpServers.genko) and Hermes Agent's ~/.hermes/config.yaml (its mcp_servers.genko).
// Claude Code and the other clients keep Python's way (a command to run, or a command to register).
//
// Nothing is written without the person: propose() reads the file and says what would change — only Genko's entry is
// shown, the client's other servers and settings by name only (they can hold keys and tokens) —, where the copy of
// the file goes, or why the file cannot be changed safely; apply() writes the copy, then the new file in place;
// undo() puts the file back as it was. The rest of the file is kept: the other servers, the other settings, and in
// Genko's entry the keys Genko does not set (env, timeout, …).

namespace genko::app::client_config {

namespace fs = std::filesystem;

// The file a client keeps its MCP servers in on this computer: Claude Desktop's in %APPDATA%\Claude (Windows),
// ~/Library/Application Support/Claude (macOS), $XDG_CONFIG_HOME/Claude or ~/.config/Claude (elsewhere); Hermes Agent's
// ~/.hermes/config.yaml. None for a client whose settings are not a file Genko writes.
std::optional<fs::path> default_file(const QString& client);

struct Proposal {
    QString client;
    fs::path file;
    bool exists = false;        // the file is there now
    QByteArray before;          // its bytes now
    QByteArray after;           // the bytes it would have
    QString entry_before;       // Genko's server in the file now (command and args; empty: none)
    QString entry_after;        // as it would be
    QStringList kept;           // what stays as it is, by name (the other servers, the other settings)
    bool same = false;          // the file already starts Genko this way: nothing to write
    fs::path backup;            // where the copy of the file goes (empty: there is no file to copy)
    QString refusal;            // why the file cannot be changed safely (nothing is written then)
};
// What writing Genko's server (`server`: the program, then its arguments) into `file` would change.
Proposal propose(const QString& client, const fs::path& file, const QStringList& server);

struct Applied {
    QString client;
    fs::path file;
    fs::path backup;     // empty: the file was made new
    QByteArray written;  // what was written
};
// Write it: the copy of the file first (its permissions kept), then the new file in place. core::Error("io") with the
// reason (in Japanese) when it cannot, or when the file changed after the proposal was made; nothing is changed then.
Applied apply(const Proposal& proposal);
// The file still holds what apply() wrote.
bool untouched(const Applied& applied);
// The file put back as it was before apply() (removed, when apply() made it). When it changed since, its present
// bytes are copied aside first (returned; empty when not needed). core::Error("io") when it cannot.
fs::path undo(const Applied& applied);
// The copies made of a file, newest first.
std::vector<fs::path> backups(const fs::path& file);
// The file names Genko's server (any command): the client is set up.
bool configured(const QString& client, const std::optional<fs::path>& file);

}  // namespace genko::app::client_config
