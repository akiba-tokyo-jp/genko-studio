#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/json.hpp"

// AI と作る (Python's genko/app/ai_link.py, its Qt-free part): how to connect an AI (Claude Desktop, Claude Code,
// Hermes Agent, others) to Genko, which AI has worked on the book lately, and what to say to it first. Genko does not
// start the AI: the AI's own settings start `genko mcp`, and the text to paste there is made here, with this computer's
// paths already filled in.
//
// C++ adds what the person needs to see the connection (SPEC AI-01, improvement I07): the connection's state in one
// word (未設定・設定済み・未接続・接続済み・処理中・人の確認待ち・失敗) from the facts it is made of — the client's settings
// file, the last test connection, the agents' calls recorded in the book (studio/logs/tools.jsonl), who wrote it lately
// (studio/presence.json), its background jobs and the requests waiting for a person — and which book and which
// conversation (MCP `session`) the AI is asked to work in.

namespace genko::app::ai_link {

namespace fs = std::filesystem;

struct Client {
    const char* key;
    const char* label;
};
// CLIENTS: claude-desktop, claude-code, hermes, other (in Python's order and words).
const std::vector<Client>& clients();
// The agent name a client's settings give the MCP server: ai:<client>, ai:agent for "other".
QString agent_of(const QString& client);

inline constexpr double kRecentSeconds = 5 * 60;  // (an AI that wrote in the last minutes is "working")

// How an AI's settings start Genko's MCP server on this computer: this build's genko, by its full path (Python: the
// genko on PATH, else its own Python with -m genko; C++ names itself so the AI talks to this build). Tests set another.
QStringList command();
void set_command_for_tests(const std::optional<QStringList>& base);

// str(Path(text.strip() or ".")): the folder as Python writes it (no doubled or trailing separators, no "." parts).
QString root_text(const QString& typed);

struct Snippet {
    QString where;   // where to paste it (in the person's words)
    QString text;    // what to paste
    QStringList server;  // the MCP server the client starts: the program, then its arguments
};
// snippet(client, root): what to paste for one AI client (root as root_text gives it).
Snippet snippet(const QString& client, const QString& root);

// What to say to the AI first (the book's folder name filled in). C++: with a conversation chosen, one more sentence
// asks the AI to pass it as every tool's session (without one, Python's words exactly).
QString first_request(const std::optional<fs::path>& book, const QString& session = {});

struct RecentAi {
    std::string actor;
    std::int64_t minutes_ago = 0;
    bool working = false;
};
// The AIs that wrote this book, newest first.
std::vector<RecentAi> recent_ai(const std::optional<fs::path>& book, std::optional<double> now = std::nullopt);
// ai:hermes/9204 → hermes（9204）.
QString ai_name(const QString& actor);
// For the status line: ("AI: hermes 作業中", true) / ("AI: hermes 12 分前", false) / ("AI と作る", false).
std::pair<QString, bool> status_words(const std::optional<fs::path>& book, std::optional<double> now = std::nullopt);
// _ago: 12 分前 / 3 時間前 / 2 日前.
QString ago(std::int64_t minutes);
// The folder the AI may open books in: this book's folder's parent, else the last book's, else home.
fs::path default_root(const std::optional<fs::path>& book);

// The conversations (MCP sessions) of this client's agent seen in the book (presence), newest first: "9204" for
// ai:hermes/9204.
QStringList conversations(const std::optional<fs::path>& book, const QString& client);
// The session check of `genko mcp` (presence.actor_for): empty when the name can be used, else why not (Python's words).
QString session_problem(const QString& session);

// --- the connection's state (SPEC AI-01) ------------------------------------------------------------------------------

enum class Link { Unset, Configured, Connected, Working, Waiting, Failed };
// 未設定 / 設定済み・未接続 / 接続済み / 処理中 / 人の確認待ち / 失敗
QString link_words(Link link);

// The last test connection: Genko's own MCP server started as the client's settings start it, asked who it is and
// which tools it has (initialize, tools/list), then stopped. Nothing is generated, charged or written.
struct Probe {
    bool ok = false;
    QDateTime at;
    QString words;      // what happened, in the person's words
    QString detail;     // more (the server's last lines on stderr), never a token
    QString server;     // serverInfo.name and version
    QString protocol;   // the protocol version agreed
    int tools = 0;
    QStringList command;  // what was started
};

// The last agent tool call recorded in the book (studio/logs/tools.jsonl's last line).
struct ToolCall {
    double at = 0;
    QString actor;
    QString tool;
    bool ok = true;
    QString error;  // the error, else the error codes
};
// The last call (of any agent; of one conversation when `actor` is given: ai:hermes/9204), read from the end of the log
// only.
std::optional<ToolCall> last_call(const fs::path& book, const QString& actor = {});
// The book's background jobs still running (studio/jobs: running and beating).
int running_jobs(const fs::path& book, std::optional<double> now = std::nullopt);

struct Facts {
    bool configured = false;            // the client's settings name Genko's server (or the person said so)
    std::optional<Probe> probe;         // the last test connection
    std::optional<ToolCall> call;       // the last agent call in the book
    bool working = false;               // an AI wrote in the last minutes, or a job runs
    int waiting = 0;                    // requests waiting for a person (the approval box)
};
// The state in one word: 失敗 (the latest of the test and the last call failed) before 人の確認待ち, 処理中, 接続済み (a
// test that answered, or a call the book recorded), 設定済み・未接続 and 未設定.
Link link_state(const Facts& facts);

// The last test connection made in this run of the app (kept for the dialog and the status line).
const std::optional<Probe>& last_probe();
void set_last_probe(const Probe& probe);

}  // namespace genko::app::ai_link
