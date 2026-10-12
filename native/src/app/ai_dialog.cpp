#include "app/ai_dialog.hpp"

#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFile>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/look.hpp"
#include "app/main_window.hpp"
#include "app/mcp_probe.hpp"
#include "app/path_label.hpp"
#include "app/review_model.hpp"
#include "app/theme.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyops.hpp"
#include "storage/reader.hpp"

namespace genko::app {

namespace fs = std::filesystem;

namespace {

QString qpath(const fs::path& path) { return QDir::toNativeSeparators(QString::fromStdString(core::path_to_utf8(path))); }
fs::path fpath(const QString& text) { return core::path_from_utf8(text.toStdString()); }

std::map<std::string, client_config::Applied>& applied_store() {
    static std::map<std::string, client_config::Applied> store;
    return store;
}

QString clock_text(const QDateTime& at) { return at.toString(QStringLiteral("HH:mm")); }
QString clock_text(double seconds) {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(seconds * 1000)).toString(QStringLiteral("M/d HH:mm"));
}

QString book_name(const fs::path& book) {
    const fs::path last = book.has_filename() ? book.filename() : book.parent_path().filename();
    return QString::fromStdString(core::path_to_utf8(last));
}

// The requests of a book that is not open in a window, read from its folder (its pictures left unread), by its
// project.json's time.
int waiting_on_disk(const fs::path& book) {
    static std::map<std::string, std::pair<QDateTime, int>> cache;
    const QFileInfo info(qpath(book / "project.json"));
    if (!info.exists()) return -1;
    const std::string key = core::path_to_utf8(book);
    if (const auto it = cache.find(key); it != cache.end() && it->second.first == info.lastModified()) return it->second.second;
    try {
        storage::LoadOptions options;
        options.verify_hashes = false;
        options.assets_of_page = 0;
        const int count = static_cast<int>(review_model::inbox(storage::load_document(book, options).document).size());
        cache[key] = {info.lastModified(), count};
        return count;
    } catch (const std::exception&) {
        return -1;
    }
}

}  // namespace

std::optional<client_config::Applied> last_applied(const fs::path& file) {
    const auto it = applied_store().find(core::path_to_utf8(file));
    if (it == applied_store().end()) return std::nullopt;
    return it->second;
}

void remember_applied(const client_config::Applied& applied) { applied_store()[core::path_to_utf8(applied.file)] = applied; }

void forget_applied(const fs::path& file) { applied_store().erase(core::path_to_utf8(file)); }

// --- 設定ファイルに書き込む ------------------------------------------------------------------------------------------

ConfigChangeDialog::ConfigChangeDialog(QWidget* parent, const client_config::Proposal& proposal) : QDialog(parent) {
    setWindowTitle(QStringLiteral("設定ファイルに書き込む"));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    auto* rows = look::form();
    file = new PathLabel;
    file->set_path(qpath(proposal.file));
    rows->addRow(QStringLiteral("書き込むファイル"), file);
    before = new QLabel(proposal.exists ? (proposal.entry_before.isEmpty() ? QStringLiteral("（Genko の設定はまだありません）") : proposal.entry_before)
                                        : QStringLiteral("（ファイルがまだありません。新しく作ります）"));
    before->setFont(mono);
    before->setTextInteractionFlags(Qt::TextSelectableByMouse);
    before->setWordWrap(true);
    rows->addRow(QStringLiteral("今の Genko の設定"), before);
    after = new QLabel(proposal.entry_after);
    after->setFont(mono);
    after->setTextInteractionFlags(Qt::TextSelectableByMouse);
    after->setWordWrap(true);
    rows->addRow(QStringLiteral("書き込む内容"), after);
    kept = new QLabel(proposal.kept.isEmpty() ? QStringLiteral("（ほかの設定はありません）")
                                              : proposal.kept.join(QLatin1Char('\n')) + QStringLiteral("\n（中身はそのまま残し、ここには名前だけを出します）"));
    kept->setWordWrap(true);
    rows->addRow(QStringLiteral("そのまま残すもの"), kept);
    backup = new PathLabel;
    if (proposal.backup.empty()) {
        backup->setText(QStringLiteral("（元のファイルが無いので要りません）"));
    } else {
        backup->set_path(qpath(proposal.backup));
    }
    rows->addRow(QStringLiteral("元のファイルの写し"), backup);
    look::quiet_labels(rows);
    auto* body = new QVBoxLayout;
    body->addLayout(rows);
    auto* note = theme::hint(new QLabel(QStringLiteral("書き込んだ後で AI のアプリを立ち上げ直すと、Genko の道具が使えるようになります。"
                                                       "「AI と作る」の「元に戻す」で、書き込む前のファイルに戻せます。")));
    note->setWordWrap(true);
    body->addWidget(note);
    auto* buttons = new QDialogButtonBox;
    write_button = buttons->addButton(QStringLiteral("書き込む"), QDialogButtonBox::AcceptRole);
    cancel_button = buttons->addButton(QStringLiteral("取りやめ"), QDialogButtonBox::RejectRole);
    theme::primary(write_button);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    look::frame(this, look::header(QStringLiteral("設定ファイルに書き込む"), QStringLiteral("書き込む前に、変わるところと元のファイルの写しの置き場所を確かめます。")),
                body, nullptr, look::footer(buttons));
    look::fit_to_screen(this, QSize(720, 480));
}

// --- AI と作る ----------------------------------------------------------------------------------------------------------

AiDialog::AiDialog(QWidget* parent, std::optional<fs::path> book, AiTarget* target)
    : QDialog(parent), book_(std::move(book)), target_(target != nullptr ? target : &own_target_) {
    setWindowTitle(QStringLiteral("AI と作る"));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    client = new QComboBox;
    for (const ai_link::Client& c : ai_link::clients()) client->addItem(QString::fromUtf8(c.label), QString::fromLatin1(c.key));
    const int chosen = client->findData(settings()->value(QStringLiteral("ai/client"), QString()).toString());  // (the AI chosen last time)
    if (chosen >= 0) client->setCurrentIndex(chosen);
    connect(client, &QComboBox::currentIndexChanged, this, [this](int) {
        settings()->setValue(QStringLiteral("ai/client"), client->currentData().toString());
        copied_ = false;
        fill_sessions();
        fill();
    });
    root = new QLineEdit(qpath(ai_link::default_root(book_)));
    root->setToolTip(QStringLiteral("AI が原稿を開いてよいフォルダ。この中の .genko の原稿だけを扱います"));
    connect(root, &QLineEdit::textChanged, this, [this](const QString&) {
        fill_books();
        fill();
    });
    auto* choose = new QPushButton(QStringLiteral("選ぶ…"));
    connect(choose, &QPushButton::clicked, this, &AiDialog::choose_root);
    auto* root_row = new QHBoxLayout;
    root_row->addWidget(root, 1);
    root_row->addWidget(choose);
    where = new QLabel;
    where->setWordWrap(true);
    theme::role(where, "hint");
    text = new QPlainTextEdit;
    text->setReadOnly(true);
    text->setFont(mono);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);  // (a path broken over two lines reads wrong)
    text->setMinimumHeight(120);
    text->setAccessibleName(QStringLiteral("AI の設定に貼る文"));
    copy = new QPushButton(QStringLiteral("設定をコピー"));
    copy->setIcon(icons::icon("duplicate"));
    connect(copy, &QPushButton::clicked, this, [this] {
        copy_text(text->toPlainText(), copy, QStringLiteral("設定をコピー"));
        copied_ = true;
        fill();
    });
    auto* steps = new QLabel(QStringLiteral("1. 上の文を AI の設定に貼る　2. AI を立ち上げ直す　3. 下の「最初に頼むこと」を AI に送る"));
    steps->setWordWrap(true);
    theme::role(steps, "hint");
    // (C++: the client's settings file written after the person has seen the change, put back, and a test connection)
    config_state = new QLabel;
    config_state->setWordWrap(true);
    config_path = new PathLabel;
    write_button = new QPushButton(QStringLiteral("設定ファイルに書き込む…"));
    write_button->setToolTip(QStringLiteral("変わるところと元のファイルの写しの置き場所を見せてから、確かめた後に書き込みます"));
    connect(write_button, &QPushButton::clicked, this, &AiDialog::write_config);
    undo_button = new QPushButton(QStringLiteral("元に戻す"));
    undo_button->setToolTip(QStringLiteral("設定ファイルを、Genko が書き込む前の状態に戻します"));
    connect(undo_button, &QPushButton::clicked, this, &AiDialog::undo_config);
    test_button = new QPushButton(QStringLiteral("テスト接続"));
    test_button->setToolTip(QStringLiteral("この設定で Genko の MCP サーバーを起動し、応答と道具の数だけを確かめます（生成・課金・原稿の変更はしません）"));
    connect(test_button, &QPushButton::clicked, this, &AiDialog::test_connection);
    applied_note = new QLabel;
    applied_note->setWordWrap(true);
    applied_note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    theme::role(applied_note, "hint");
    probe_ = new McpProbe(this);
    connect(probe_, &McpProbe::finished, this, &AiDialog::probe_done);

    auto* rows = look::form();
    rows->addRow(look::section(QStringLiteral("1. AI をつなぐ")));
    rows->addRow(QStringLiteral("使う AI"), client);
    rows->addRow(QStringLiteral("原稿のフォルダ"), root_row);
    look::quiet_labels(rows);
    auto* body = new QVBoxLayout;
    body->addLayout(rows);
    body->addWidget(where);
    body->addWidget(text, 1);
    auto* copy_row = new QHBoxLayout;
    copy_row->addWidget(steps, 1);
    copy_row->addWidget(copy);
    body->addLayout(copy_row);
    auto* config_row = new QHBoxLayout;
    config_row->addWidget(write_button);
    config_row->addWidget(undo_button);
    config_row->addStretch(1);
    config_row->addWidget(test_button);
    body->addWidget(config_state);
    body->addWidget(config_path);
    body->addLayout(config_row);
    body->addWidget(applied_note);
    body->addWidget(look::section(QStringLiteral("2. 最初に頼むこと")));
    // (C++: which book and which conversation the AI is asked to work in)
    book_pick = new QComboBox;
    book_pick->setToolTip(QStringLiteral("AI に作業してもらう原稿"));
    connect(book_pick, &QComboBox::currentIndexChanged, this, [this](int) {
        if (filling_) return;
        fill_sessions();
        target_changed();
    });
    session_pick = new QComboBox;
    session_pick->setEditable(true);
    session_pick->setToolTip(QStringLiteral("同じ AI を複数の会話（チャットのスレッドなど）で使うときの、この会話の名前。"
                                             "AI はどの道具にもこの名前を session として渡し、変更はこの名前で記録されます"));
    session_pick->lineEdit()->setPlaceholderText(QStringLiteral("会話の名前（例 9204。一つの会話だけなら空のまま）"));
    connect(session_pick, &QComboBox::currentTextChanged, this, [this](const QString&) {
        if (!filling_) target_changed();
    });
    session_note = new QLabel;
    session_note->setWordWrap(true);
    theme::role(session_note, "error");
    session_note->hide();
    auto* target_rows = look::form();
    target_rows->addRow(QStringLiteral("原稿"), book_pick);
    target_rows->addRow(QStringLiteral("会話"), session_pick);
    look::quiet_labels(target_rows);
    body->addLayout(target_rows);
    body->addWidget(session_note);
    target_label = new QLabel;
    target_label->setWordWrap(true);
    theme::role(target_label, "hint");
    body->addWidget(target_label);
    ask = new QPlainTextEdit(ai_link::first_request(book_));
    ask->setMaximumHeight(72);
    ask->setAccessibleName(QStringLiteral("AI に最初に送る文"));
    ask_copy = new QPushButton(QStringLiteral("文をコピー"));
    ask_copy->setIcon(icons::icon("duplicate"));
    connect(ask_copy, &QPushButton::clicked, this, [this] { copy_text(ask->toPlainText(), ask_copy, QStringLiteral("文をコピー")); });
    auto* ask_row = new QHBoxLayout;
    ask_row->addWidget(ask, 1);
    ask_row->addWidget(ask_copy, 0, Qt::AlignTop);
    body->addLayout(ask_row);

    auto* side_widget = new QWidget;
    auto* side = new QVBoxLayout(side_widget);
    side->setContentsMargins(0, 0, 0, 0);
    state_word = new QLabel;
    state_word->setObjectName(QStringLiteral("aiState"));
    theme::role(state_word, "heading");
    side->addWidget(state_word);
    state_rows = new QLabel;
    state_rows->setWordWrap(true);
    state_rows->setTextFormat(Qt::RichText);
    state_rows->setTextInteractionFlags(Qt::TextSelectableByMouse);
    side->addWidget(state_rows);
    people = new QLabel;
    people->setWordWrap(true);
    people->setTextFormat(Qt::RichText);
    side->addWidget(people);
    auto* how = new QLabel(QStringLiteral("AI の作業は、開いている画面にすぐ出ます。人が決めること（ネーム・絵・書き出しの承認）は"
                                          "右の「承認箱」に届きます。AI が変えたものは「履歴」に AI の名前付きで残ります。"));
    how->setWordWrap(true);
    theme::role(how, "hint");
    side->addWidget(how);
    side->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    look::frame(this,
                look::header(QStringLiteral("AI と作る"), QStringLiteral("Claude などの AI が、Genko の道具でネームから仕上げまで進めます。"
                                                                     "あなたは画面で見守り、承認箱で決め、好きなところを自分で直せます。")),
                body, look::card(side_widget, QStringLiteral("この原稿の AI")), look::footer(buttons));
    look::fit_to_screen(this, QSize(900, 600));
    fill_books();
    fill_sessions();
    target_changed();
    refresh_.setInterval(5000);  // (the AI's work and calls as they come)
    connect(&refresh_, &QTimer::timeout, this, &AiDialog::fill);
    refresh_.start();
}

AiDialog::~AiDialog() = default;

QString AiDialog::client_key() const { return client->currentData().toString(); }
QString AiDialog::root_typed() const { return ai_link::root_text(root->text()); }

std::optional<fs::path> AiDialog::chosen_book() const {
    const QString data = book_pick->currentData().toString();
    if (data.isEmpty()) return std::nullopt;
    return fpath(data);
}

QString AiDialog::chosen_session() const { return QString::fromStdString(core::py_strip(session_pick->currentText().toStdString())); }

std::optional<fs::path> AiDialog::config_file() const { return client_config::default_file(client_key()); }

int AiDialog::waiting_for(const std::optional<fs::path>& book) const {
    if (!book) return 0;
    // a book open in a window: its count in memory
    for (MainWindow* window : documents::windows()) {
        for (const Document& doc : window->documents()) {
            if (doc.session->path() && *doc.session->path() == *book) return static_cast<int>(review_model::inbox(doc.session->document()).size());
        }
    }
    return waiting_on_disk(*book);
}

std::optional<ai_link::Probe> AiDialog::probe_now() const {
    const auto& probe = ai_link::last_probe();
    if (!probe || probe->command != ai_link::snippet(client_key(), root_typed()).server) return std::nullopt;  // (another setup's test)
    return probe;
}

ai_link::Facts AiDialog::facts() const {
    ai_link::Facts f;
    const QString key = client_key();
    if (const auto file = config_file()) {
        f.configured = client_config::configured(key, file);
    } else {
        f.configured = copied_;
    }
    f.probe = probe_now();
    if (const auto book = chosen_book()) {
        const QString session = chosen_session();
        const QString actor = session.isEmpty() || !ai_link::session_problem(session).isEmpty() ? QString() : ai_link::agent_of(key) + QLatin1Char('/') + session;
        f.call = ai_link::last_call(*book, actor);
        bool working = false;
        for (const auto& ai : ai_link::recent_ai(book)) working = working || ai.working;
        f.working = working || ai_link::running_jobs(*book) > 0;
        f.waiting = std::max(0, waiting_for(book));
    }
    return f;
}

ai_link::Link AiDialog::link() const { return ai_link::link_state(facts()); }

void AiDialog::fill() {
    const QString key = client_key();
    const ai_link::Snippet snippet = ai_link::snippet(key, root_typed());
    where->setText(snippet.where);
    if (text->toPlainText() != snippet.text) text->setPlainText(snippet.text);
    // the settings file
    const auto file = config_file();
    write_button->setVisible(file.has_value());
    undo_button->setVisible(file.has_value());
    config_path->setVisible(file.has_value());
    if (file) {
        config_path->set_path(qpath(*file), QStringLiteral("設定ファイル: "));
        const bool there = QFileInfo::exists(qpath(*file));
        const bool has = client_config::configured(key, file);
        config_state->setText(!there ? QStringLiteral("設定ファイルはまだありません。「設定ファイルに書き込む…」で作るか、上の文を手で貼ります。")
                              : has  ? QStringLiteral("設定ファイルに Genko の設定があります。")
                                     : QStringLiteral("設定ファイルに Genko の設定はまだありません。"));
        undo_button->setEnabled(last_applied(*file).has_value() || newest_backup_differs(*file));
    } else {
        config_state->setText(copied_ ? QStringLiteral("設定をコピーしました（この AI の設定は Genko からは確かめられません）。")
                                      : QStringLiteral("この AI の設定は、上の文をコピーして使います（Genko からは書き換えません）。"));
    }
    // the state and its facts
    const ai_link::Facts f = facts();
    const ai_link::Link state = ai_link::link_state(f);
    state_word->setText(ai_link::link_words(state));
    theme::role(state_word, state == ai_link::Link::Failed ? "error" : state == ai_link::Link::Waiting ? "badge-warn" : "heading");
    QStringList rows;
    const auto row = [&rows](const QString& name, const QString& value) {
        rows << QStringLiteral("<b>%1</b>: %2").arg(name.toHtmlEscaped(), value.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    };
    row(QStringLiteral("設定"), f.configured ? QStringLiteral("済み") : QStringLiteral("まだ"));
    if (f.probe) {
        row(QStringLiteral("テスト接続"), QStringLiteral("%1（%2）%3").arg(f.probe->ok ? QStringLiteral("成功") : QStringLiteral("失敗"), clock_text(f.probe->at), f.probe->words) +
                                            (f.probe->detail.isEmpty() ? QString() : QStringLiteral("\n") + f.probe->detail));
    } else {
        row(QStringLiteral("テスト接続"), QStringLiteral("まだしていません"));
    }
    const auto book = chosen_book();
    const QString session = chosen_session();
    row(QStringLiteral("依頼先"), book ? QStringLiteral("原稿「%1」・%2").arg(book_name(*book), session.isEmpty() ? QStringLiteral("会話の名前なし") : QStringLiteral("会話「%1」").arg(session))
                                     : QStringLiteral("原稿を選んでいません"));
    if (f.call) {
        row(QStringLiteral("最後の呼び出し"), QStringLiteral("%1 %2 の %3: %4").arg(clock_text(f.call->at), ai_link::ai_name(f.call->actor), f.call->tool,
                                                                                f.call->ok ? QStringLiteral("成功") : QStringLiteral("失敗 — %1").arg(f.call->error)));
    } else {
        row(QStringLiteral("最後の呼び出し"), book ? QStringLiteral("まだありません（AI はこの原稿で道具を使っていません）") : QStringLiteral("—"));
    }
    row(QStringLiteral("作業"), f.working ? QStringLiteral("処理中") : QStringLiteral("止まっています"));
    row(QStringLiteral("確認待ち"), f.waiting > 0 ? QStringLiteral("%1 件（承認箱）").arg(f.waiting) : QStringLiteral("なし"));
    state_rows->setText(rows.join(QStringLiteral("<br>")));
    // who worked on the book (Python's card)
    const auto found = ai_link::recent_ai(book);
    QString words;
    if (!book) {
        words = QStringLiteral("原稿を開くと、その原稿で動いた AI がここに出ます。");
    } else if (found.empty()) {
        words = QStringLiteral("この原稿では、まだ AI が作業していません。");
    } else {
        QStringList lines;
        for (std::size_t i = 0; i < found.size() && i < 6; ++i) {
            const QString mark = found[i].working ? QStringLiteral("<b>● 作業中</b>") : ai_link::ago(found[i].minutes_ago);
            lines << QStringLiteral("%1 — %2").arg(ai_link::ai_name(QString::fromStdString(found[i].actor)).toHtmlEscaped(), mark);
        }
        words = lines.join(QStringLiteral("<br>"));
    }
    people->setText(words);
}

void AiDialog::fill_books() {
    filling_ = true;
    const std::optional<fs::path> keep = chosen_book() ? chosen_book() : target_->book ? target_->book : book_;
    book_pick->clear();
    std::vector<fs::path> books;
    if (book_) books.push_back(*book_);
    const QDir folder(QString::fromStdString(core::path_to_utf8(fpath(root_typed()))));
    for (const QFileInfo& entry : folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (!QFileInfo::exists(entry.filePath() + QStringLiteral("/project.json"))) continue;
        const fs::path path = fpath(entry.absoluteFilePath());
        if (std::none_of(books.begin(), books.end(), [&](const fs::path& p) { return qpath(p) == qpath(path); })) books.push_back(path);
    }
    if (keep && std::none_of(books.begin(), books.end(), [&](const fs::path& p) { return qpath(p) == qpath(*keep); })) books.push_back(*keep);
    for (const fs::path& path : books) book_pick->addItem(book_name(path), QString::fromStdString(core::path_to_utf8(path)));
    if (books.empty()) book_pick->addItem(QStringLiteral("（原稿なし: 新しい原稿）"), QString());
    if (keep) {
        for (int i = 0; i < book_pick->count(); ++i) {
            if (qpath(fpath(book_pick->itemData(i).toString())) == qpath(*keep)) book_pick->setCurrentIndex(i);
        }
    }
    filling_ = false;
}

void AiDialog::fill_sessions() {
    filling_ = true;
    const QString keep = session_pick->count() > 0 || !session_pick->currentText().isEmpty() ? session_pick->currentText() : target_->session;
    session_pick->clear();
    session_pick->addItem(QString());
    for (const QString& name : ai_link::conversations(chosen_book(), client_key())) session_pick->addItem(name);
    session_pick->setCurrentText(keep);
    filling_ = false;
}

void AiDialog::target_changed() {
    const QString session = chosen_session();
    const QString problem = session.isEmpty() ? QString() : ai_link::session_problem(session);
    session_note->setText(problem);
    session_note->setVisible(!problem.isEmpty());
    const auto book = chosen_book();
    if (problem.isEmpty()) {
        target_->book = book;
        target_->session = session;
    }
    const QString used = problem.isEmpty() ? session : QString();
    target_label->setText(book ? QStringLiteral("依頼先: %1%2").arg(qpath(*book), used.isEmpty() ? QString() : QStringLiteral("（会話「%1」）").arg(used))
                               : QStringLiteral("依頼先: 新しい原稿（AI が作ります）"));
    ask->setPlainText(ai_link::first_request(book, used));
    fill();
}

void AiDialog::choose_root() {
    const QString folder = ask::existing_dir(this, QStringLiteral("AI が原稿を開いてよいフォルダ"), root->text());
    if (!folder.isEmpty()) root->setText(folder);
}

void AiDialog::copy_text(const QString& value, QPushButton* button, const QString& label) {
    QGuiApplication::clipboard()->setText(value);
    button->setText(QStringLiteral("コピーしました"));
    QTimer::singleShot(1600, button, [button, label] { button->setText(label); });
}

void AiDialog::write_config() {
    const auto file = config_file();
    if (!file) return;
    const QString key = client_key();
    const client_config::Proposal proposal = client_config::propose(key, *file, ai_link::snippet(key, root_typed()).server);
    if (!proposal.refusal.isEmpty()) {
        ask::warning(this, QStringLiteral("設定ファイルに書き込む"), proposal.refusal);
        applied_note->setText(QStringLiteral("書き込みませんでした: %1").arg(proposal.refusal));
        return;
    }
    if (proposal.same) {
        ask::warning(this, QStringLiteral("設定ファイルに書き込む"), QStringLiteral("設定ファイルには、すでに同じ設定があります（書き込むものはありません）。"));
        fill();
        return;
    }
    ConfigChangeDialog dialog(this, proposal);
    if (ask::exec(&dialog) != QDialog::Accepted) {
        applied_note->setText(QStringLiteral("書き込みませんでした（取りやめ）。設定ファイルはそのままです。"));
        return;
    }
    try {
        const client_config::Applied applied = client_config::apply(proposal);
        remember_applied(applied);
        applied_note->setText(applied.backup.empty()
                                  ? QStringLiteral("設定ファイルを作りました。AI のアプリを立ち上げ直してください。")
                                  : QStringLiteral("設定ファイルに書き込みました。元のファイルの写し: %1\nAI のアプリを立ち上げ直してください。").arg(qpath(applied.backup)));
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("設定ファイルに書き込む"), QString::fromUtf8(error.what()));
        applied_note->setText(QStringLiteral("書き込めませんでした: %1").arg(QString::fromUtf8(error.what())));
    }
    fill();
}

void AiDialog::undo_config() {
    const auto file = config_file();
    if (!file) return;
    client_config::Applied applied;
    if (const auto last = last_applied(*file)) {
        applied = *last;
        const QString question = client_config::untouched(applied)
                                     ? QStringLiteral("設定ファイルを、Genko が書き込む前の状態に戻しますか？")
                                     : QStringLiteral("設定ファイルは、Genko が書き込んだ後に変わっています（AI のアプリが書き換えたのかもしれません）。"
                                                      "今のファイルの写しを残してから、Genko が書き込む前の状態に戻しますか？");
        if (!ask::question(this, QStringLiteral("元に戻す"), question)) return;
    } else {
        const auto copies = client_config::backups(*file);
        if (copies.empty() || !newest_backup_differs(*file)) return;  // (put back already: a second press does nothing)
        applied.client = client_key();
        applied.file = *file;
        applied.backup = copies.front();
        applied.written = QByteArray("\x01(not this run)");  // (the file is copied aside before it is replaced)
        if (!ask::question(this, QStringLiteral("元に戻す"),
                           QStringLiteral("設定ファイルを、いちばん新しい写し（%1）の状態に戻しますか？ 今のファイルは写しを残します。")
                               .arg(QString::fromStdString(core::path_to_utf8(copies.front().filename()))))) {
            return;
        }
    }
    try {
        const fs::path aside = client_config::undo(applied);
        forget_applied(*file);
        applied_note->setText(aside.empty() ? QStringLiteral("設定ファイルを元に戻しました。AI のアプリを立ち上げ直してください。")
                                            : QStringLiteral("設定ファイルを元に戻しました（戻す前のファイルの写し: %1）。AI のアプリを立ち上げ直してください。").arg(qpath(aside)));
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("元に戻す"), QString::fromUtf8(error.what()));
    }
    fill();
}

bool AiDialog::newest_backup_differs(const std::filesystem::path& file) const {
    const auto copies = client_config::backups(file);
    if (copies.empty()) return false;
    QFile now(qpath(file));
    QFile copy(qpath(copies.front()));
    if (!copy.open(QIODevice::ReadOnly)) return true;
    if (!now.open(QIODevice::ReadOnly)) return true;  // (no file now: the copy can be put back)
    return now.readAll() != copy.readAll();
}

void AiDialog::test_connection() {
    // (the folder the server is given must be there: the test starts nothing, and makes no folder, when it is not)
    const QString folder = root_typed();
    if (!QFileInfo(folder).isDir()) {
        ai_link::Probe probe;
        probe.ok = false;
        probe.at = QDateTime::currentDateTime();
        probe.words = QStringLiteral("原稿のフォルダー（%1）がありません。フォルダーを確かめてから、もう一度テスト接続します（何も起動していません）").arg(folder);
        probe.command = ai_link::snippet(client_key(), folder).server;
        ai_link::set_last_probe(probe);
        probe_done(probe);
        return;
    }
    test_button->setEnabled(false);
    test_button->setText(QStringLiteral("確かめています…"));
    probe_->start(ai_link::snippet(client_key(), root_typed()).server, probe_timeout_ms);
}

void AiDialog::probe_done(const ai_link::Probe&) {
    test_button->setEnabled(true);
    test_button->setText(QStringLiteral("テスト接続"));
    fill();
}

}  // namespace genko::app
