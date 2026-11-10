#pragma once

#include <QDialog>
#include <QTimer>

#include <filesystem>
#include <optional>

#include "app/ai_link.hpp"
#include "app/client_config.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

// AI と作る (Python's genko/app/ai_link.py AiDialog): connect an AI, see who worked on the book, and what to ask
// first. C++ adds (SPEC AI-01, improvement I07; PLAN M5): the connection's state in one word with the facts under it
// (the client's settings, the test connection, which book and conversation the AI is asked to work in, its last call
// and why it failed, its work, the requests waiting for the person); writing Genko into the client's settings file
// after the person has seen what changes and where the copy of the file goes (and putting it back); a test connection
// to Genko's own MCP server that generates nothing, charges nothing and changes no book; the book and the conversation
// (MCP `session`) the first request names.

namespace genko::app {

class McpProbe;
class PathLabel;

// The book and the conversation a window asks its AI to work in (kept by the window while it is open).
struct AiTarget {
    std::optional<std::filesystem::path> book;
    QString session;
};

// 設定ファイルに書き込む: what writing Genko into the client's settings changes, shown before anything is written.
class ConfigChangeDialog : public QDialog {
    Q_OBJECT

public:
    ConfigChangeDialog(QWidget* parent, const client_config::Proposal& proposal);

    QLabel* before = nullptr;
    QLabel* after = nullptr;
    QLabel* kept = nullptr;
    PathLabel* file = nullptr;
    PathLabel* backup = nullptr;
    QPushButton* write_button = nullptr;
    QPushButton* cancel_button = nullptr;
};

class AiDialog : public QDialog {
    Q_OBJECT

public:
    // `book`: the window's book (none: the start screen's); `target`: the window's choice of book and conversation.
    AiDialog(QWidget* parent = nullptr, std::optional<std::filesystem::path> book = std::nullopt, AiTarget* target = nullptr);
    ~AiDialog() override;

    // The client chosen ("claude-desktop" …), the root typed, the book and conversation chosen.
    QString client_key() const;
    QString root_typed() const;
    std::optional<std::filesystem::path> chosen_book() const;
    QString chosen_session() const;
    // The settings file Genko writes for the client chosen (none: a client whose settings are not a file).
    std::optional<std::filesystem::path> config_file() const;
    // The state now, from the facts (read again).
    ai_link::Link link() const;
    ai_link::Facts facts() const;
    // Refill the texts and the state (as the dialog does after each change and every few seconds).
    void fill();
    // 設定ファイルに書き込む…, 元に戻す, テスト接続.
    void write_config();
    void undo_config();
    void test_connection();
    McpProbe* probe() const { return probe_; }
    int probe_timeout_ms = 30000;
    // A book's requests waiting for the person (a book open in a window: as it is there; another, read from its folder).
    int waiting_for(const std::optional<std::filesystem::path>& book) const;

    QComboBox* client = nullptr;
    QLineEdit* root = nullptr;
    QLabel* where = nullptr;
    QPlainTextEdit* text = nullptr;
    QPushButton* copy = nullptr;
    QPushButton* write_button = nullptr;
    QPushButton* undo_button = nullptr;
    QPushButton* test_button = nullptr;
    PathLabel* config_path = nullptr;
    QLabel* config_state = nullptr;
    QLabel* applied_note = nullptr;
    QComboBox* book_pick = nullptr;
    QComboBox* session_pick = nullptr;
    QLabel* session_note = nullptr;
    QLabel* target_label = nullptr;
    QPlainTextEdit* ask = nullptr;
    QPushButton* ask_copy = nullptr;
    QLabel* state_word = nullptr;
    QLabel* state_rows = nullptr;
    QLabel* people = nullptr;

private:
    void choose_root();
    void copy_text(const QString& text, QPushButton* button, const QString& label);
    void fill_books();
    void fill_sessions();
    void target_changed();
    void probe_done(const ai_link::Probe& result);
    // The newest copy of the settings file is not what the file holds now (an undo would change something).
    bool newest_backup_differs(const std::filesystem::path& file) const;
    std::optional<ai_link::Probe> probe_now() const;

    std::optional<std::filesystem::path> book_;
    AiTarget* target_ = nullptr;
    AiTarget own_target_;
    bool copied_ = false;
    bool filling_ = false;
    McpProbe* probe_ = nullptr;
    QTimer refresh_;
};

// The settings written in this run of the app, by file (元に戻す puts the last one back).
std::optional<client_config::Applied> last_applied(const std::filesystem::path& file);
void remember_applied(const client_config::Applied& applied);
void forget_applied(const std::filesystem::path& file);

}  // namespace genko::app
