#include "app/story_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>

#include "app/ask.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "service/wording.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/pyre.hpp"
#include "render/ops_registry.hpp"
#include "render/text/fonts.hpp"
#include "core/ruby.hpp"
#include "core/type_text.hpp"

namespace genko::app {

using core::Json;

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }
std::string utf8(const QString& text) { return text.toStdString(); }

QRegularExpression pattern(const QString& text) {
    return QRegularExpression(QRegularExpression::anchoredPattern(text), QRegularExpression::UseUnicodePropertiesOption);
}

// _PAGE, _QUOTE, _COLON (matched from the start of a stripped line: re.match)
const QRegularExpression& page_marker() {
    static const QRegularExpression re =
        pattern(QStringLiteral(R"(\s*(?:[#＃]+\s*(\d+)|(\d+)\s*(?:ページ|頁|P|p)|[PpＰ]\s*(\d+)|[-ー―─]{3,}\s*(\d*))\s*)"));
    return re;
}

const QRegularExpression& quoted() {
    static const QRegularExpression re = pattern(QStringLiteral(R"(\s*([^「『:：\s][^「『:：]{0,11}?)\s*[「『](.+)[」』]\s*)"));
    return re;
}

const QRegularExpression& colon() {
    static const QRegularExpression re = pattern(QStringLiteral(R"(\s*([^「『:：\s][^「『:：]{0,11}?)\s*[:：]\s*(.+))"));
    return re;
}

// int(text) of what a person typed: its digits (any script's, as Python reads them), a sign, spaces around; none when
// it is not a whole number
std::optional<std::int64_t> py_int_text(const QString& typed) {
    const QString text = lettering::strip(typed);
    if (text.isEmpty()) return std::nullopt;
    qsizetype i = 0;
    bool negative = false;
    if (text[0] == QLatin1Char('+') || text[0] == QLatin1Char('-')) {
        negative = text[0] == QLatin1Char('-');
        ++i;
    }
    if (i >= text.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; i < text.size(); ++i) {
        const int digit = text[i].digitValue();
        if (digit < 0) return std::nullopt;
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

// str.splitlines()
QStringList split_lines(const QString& text) {
    static const QRegularExpression breaks(QStringLiteral("\\r\\n|[\\n\\r\\x{000b}\\x{000c}\\x{001c}\\x{001d}\\x{001e}\\x{0085}\\x{2028}\\x{2029}]"));
    QStringList lines = text.split(breaks);
    if (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();
    return lines;
}

bool in(QChar c, const QString& set) { return set.contains(c); }

}  // namespace

std::vector<ScriptRow> parse_script(const QString& text, std::int64_t start_page) {
    std::vector<ScriptRow> out;
    std::int64_t page = start_page;
    bool seen_marker = false;
    for (const QString& raw : split_lines(text)) {
        const QString line = lettering::strip(raw);
        if (line.isEmpty()) continue;
        if (const auto marker = page_marker().match(line); marker.hasMatch()) {
            std::optional<QString> number;
            for (int g = 1; g <= 4 && !number; ++g) {
                if (!marker.captured(g).isEmpty()) number = marker.captured(g);
            }
            if (number) {
                page = py_int_text(*number).value_or(page);
            } else if (seen_marker || !out.empty()) {
                page += 1;
            }
            seen_marker = true;
            continue;
        }
        QString speaker;
        QString body = line;
        QString balloon = QStringLiteral("speech");
        const auto quote = quoted().match(line);
        const auto said = colon().match(line);
        if (in(line.front(), QStringLiteral("（(")) && in(line.back(), QStringLiteral("）)"))) {
            body = lettering::strip(line.mid(1, line.size() - 2));
            balloon = QStringLiteral("thought");
        } else if (quote.hasMatch()) {
            speaker = lettering::strip(quote.captured(1));
            body = lettering::strip(quote.captured(2));
        } else if (said.hasMatch()) {
            speaker = lettering::strip(said.captured(1));
            body = lettering::strip(said.captured(2));
            if (QStringList{QStringLiteral("ナレ"), QStringLiteral("ナレーション"), QStringLiteral("N"), QStringLiteral("n"), QStringLiteral("Ｎ")}.contains(speaker)) {
                speaker.clear();
                balloon = QStringLiteral("narration");
            }
        } else if (in(line.front(), QStringLiteral("「『")) && in(line.back(), QStringLiteral("」』"))) {
            body = lettering::strip(line.mid(1, line.size() - 2));
        }
        if (!body.isEmpty()) out.push_back(ScriptRow{std::nullopt, page, speaker, body, balloon});
    }
    return out;
}

// --- 台本を流し込む -------------------------------------------------------------------------------------------------

PourDialog::PourDialog(QWidget* parent, std::int64_t first_page) : QDialog(parent) {
    setWindowTitle(QStringLiteral("台本を流し込む"));
    text = new QPlainTextEdit;
    text->setPlaceholderText(QStringLiteral("# 1\n太郎「おはよう」\n花子：遅いよ\n（また寝坊した…）\nナレ：翌朝\n# 2\n…"));
    start = new QSpinBox;
    start->setRange(1, 999);
    start->setValue(static_cast<int>(std::clamp<std::int64_t>(first_page, 1, 999)));
    replace = new QCheckBox(QStringLiteral("流し込むページの今の台詞を消す"));
    auto* hint = new QLabel(QStringLiteral("「# 3」「3ページ」でページを変えます（「---」だけなら次のページ）。話者「台詞」・話者：台詞・（心の声）・ナレ：… が使えます。"
                                           "\nページの中では、コマの読み順に台詞を割り振ります。"));
    hint->setWordWrap(true);
    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("ページの指定が無い台詞は")));
    row->addWidget(start);
    row->addWidget(new QLabel(QStringLiteral("ページから")));
    row->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("表に入れる"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(hint);
    layout->addWidget(text, 1);
    layout->addLayout(row);
    layout->addWidget(replace);
    layout->addWidget(buttons);
    resize(560, 480);
}

std::vector<ScriptRow> PourDialog::rows() const { return parse_script(text->toPlainText(), start->value()); }

// --- ストーリーエディター ----------------------------------------------------------------------------------------------

StoryEditor::StoryEditor(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("story_editor"));
    setWindowTitle(QStringLiteral("ストーリーエディター（全ページの台詞）"));
    table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({QStringLiteral("ページ"), QStringLiteral("話者"), QStringLiteral("台詞（｜漢字《かんじ》でルビ）"), QStringLiteral("フキダシ")});
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table->setWordWrap(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto* buttons = new QHBoxLayout;
    for (const auto& [title, name, slot] : std::initializer_list<std::tuple<const char*, const char*, std::function<void()>>>{
             {"行を追加", "story_add_row", [this] { add_row(); }},
             {"行を消す", "story_delete_rows", [this] { delete_rows(); }},
             {"↑", "story_row_up", [this] { move(-1); }},
             {"↓", "story_row_down", [this] { move(1); }},
             {"台本を流し込む…", "story_pour", [this] { pour(); }}}) {
        auto* button = new QPushButton(QString::fromUtf8(title));
        button->setObjectName(QString::fromLatin1(name));
        connect(button, &QPushButton::clicked, this, [slot = slot] { slot(); });
        buttons->addWidget(button);
    }
    buttons->addStretch(1);
    status = new QLabel;
    auto* box = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close);
    apply_button = box->button(QDialogButtonBox::Apply);
    apply_button->setText(QStringLiteral("原稿に反映"));
    connect(apply_button, &QPushButton::clicked, this, [this] { apply(); });
    box->button(QDialogButtonBox::Close)->setText(QStringLiteral("閉じる"));
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("台詞を書き換えて「原稿に反映」。新しい行はそのページのコマに読み順で入ります。行の順番がそのページの読み順です。")));
    layout->addWidget(table, 1);
    layout->addLayout(buttons);
    layout->addWidget(status);
    layout->addWidget(box);
    resize(900, 620);
    load();
}

void StoryEditor::load() {
    table->setRowCount(0);
    deleted.clear();
    const core::Document& book = window_->book();
    for (const auto& page : book.pages) {
        for (const core::StoryLine* line : book.story_for_page(page->index)) {
            append(ScriptRow{line->id, page->index.is_int() ? page->index.int_value() : static_cast<std::int64_t>(page->index.value()), qs(line->speaker),
                             lettering::with_marks(*line), line->balloon.empty() ? QStringLiteral("speech") : qs(line->balloon)});
        }
    }
    count();
}

void StoryEditor::append(const ScriptRow& row, std::optional<int> at) {
    const int r = at.value_or(table->rowCount());
    table->insertRow(r);
    auto* page = new QTableWidgetItem(QString::number(row.page));
    page->setData(Qt::UserRole, row.id ? QVariant(qs(*row.id)) : QVariant());
    if (row.id) {
        page->setFlags(page->flags() & ~Qt::ItemIsEditable);
        page->setToolTip(QStringLiteral("置いてある台詞のページは変えられません（新しい行で書き直します）"));
    }
    table->setItem(r, 0, page);
    table->setItem(r, 1, new QTableWidgetItem(row.speaker));
    table->setItem(r, 2, new QTableWidgetItem(row.text));
    auto* combo = new QComboBox;
    for (const auto& [key, label] : lettering::kinds()) combo->addItem(label, key);
    combo->setCurrentIndex(std::max(0, combo->findData(row.balloon.isEmpty() ? QStringLiteral("speech") : row.balloon)));
    table->setCellWidget(r, 3, combo);
}

ScriptRow StoryEditor::row(int r) const {
    const QTableWidgetItem* page = table->item(r, 0);
    ScriptRow out;
    const QVariant id = page != nullptr ? page->data(Qt::UserRole) : QVariant();
    if (id.isValid() && !id.isNull() && !id.toString().isEmpty()) out.id = utf8(id.toString());
    out.page = page != nullptr ? py_int_text(page->text()).value_or(0) : 0;
    out.speaker = table->item(r, 1) != nullptr ? lettering::strip(table->item(r, 1)->text()) : QString();
    out.text = table->item(r, 2) != nullptr ? lettering::strip(table->item(r, 2)->text()) : QString();
    const auto* combo = qobject_cast<const QComboBox*>(table->cellWidget(r, 3));
    out.balloon = combo != nullptr ? combo->currentData().toString() : QStringLiteral("speech");
    return out;
}

std::vector<ScriptRow> StoryEditor::rows() const {
    std::vector<ScriptRow> out;
    for (int r = 0; r < table->rowCount(); ++r) out.push_back(row(r));
    return out;
}

void StoryEditor::count() {
    const auto all = rows();
    const auto fresh = std::count_if(all.begin(), all.end(), [](const ScriptRow& r) { return !r.id; });
    status->setText(QStringLiteral("台詞 %1 行（新しい行 %2、消す行 %3）").arg(all.size()).arg(fresh).arg(deleted.size()));
}

void StoryEditor::add_row() {
    const int current = table->currentRow();
    const core::Page* shown = window_->current_page();
    const std::int64_t page = current >= 0 ? row(current).page
                                           : (shown != nullptr ? (shown->index.is_int() ? shown->index.int_value() : static_cast<std::int64_t>(shown->index.value())) : 1);
    const int at = current >= 0 ? current + 1 : table->rowCount();
    append(ScriptRow{std::nullopt, page, QString(), QString(), QStringLiteral("speech")}, at);
    table->setCurrentCell(at, 2);
    count();
}

void StoryEditor::delete_rows() {
    std::set<int> chosen;
    for (const QModelIndex& index : table->selectionModel()->selectedIndexes()) chosen.insert(index.row());  // (selectedIndexes is protected in C++)
    if (chosen.empty()) chosen.insert(table->currentRow());
    for (auto it = chosen.rbegin(); it != chosen.rend(); ++it) {
        if (*it < 0) continue;
        if (const auto id = row(*it).id) deleted.insert(*id);
        table->removeRow(*it);
    }
    count();
}

void StoryEditor::move(int delta) {
    const int r = table->currentRow();
    const int target = r + delta;
    if (r < 0 || target < 0 || target >= table->rowCount()) return;
    const ScriptRow a = row(r);
    const ScriptRow b = row(target);
    if (a.page != b.page) return;  // the order is within a page
    table->removeRow(r);
    append(a, target);
    table->setCurrentCell(target, 2);
}

void StoryEditor::pour() {
    const core::Page* page = window_->current_page();
    PourDialog dialog(this, page != nullptr ? (page->index.is_int() ? page->index.int_value() : static_cast<std::int64_t>(page->index.value())) : 1);
    if (ask::exec(&dialog) != QDialog::Accepted) return;
    pour_rows(dialog.rows(), dialog.replace->isChecked());
}

void StoryEditor::pour_rows(const std::vector<ScriptRow>& poured, bool replace) {
    if (replace) {
        std::set<std::int64_t> pages;
        for (const ScriptRow& r : poured) pages.insert(r.page);
        for (int r = table->rowCount() - 1; r >= 0; --r) {
            const ScriptRow now = row(r);
            if (pages.contains(now.page)) {
                if (now.id) deleted.insert(*now.id);
                table->removeRow(r);
            }
        }
    }
    for (const ScriptRow& fresh : poured) {
        // after the last row of its page (or where the pages come in order)
        int at = table->rowCount();
        for (int r = 0; r < table->rowCount(); ++r) {
            if (row(r).page > fresh.page) {
                at = r;
                break;
            }
        }
        ScriptRow added = fresh;
        added.id.reset();
        append(added, at);
    }
    count();
}

Json StoryEditor::plan() {
    const core::Document& book = window_->book();
    std::vector<ScriptRow> all;
    for (const ScriptRow& r : rows()) {
        if (!r.text.isEmpty()) all.push_back(r);
    }
    Json ops = Json::array();
    std::int64_t want_pages = static_cast<std::int64_t>(book.pages.size());
    for (const ScriptRow& r : all) want_pages = std::max(want_pages, r.page);
    if (want_pages > static_cast<std::int64_t>(book.pages.size())) {
        ops.push_back(Json{{"op", "add_page"}, {"count", want_pages - static_cast<std::int64_t>(book.pages.size())}});
    }
    for (const std::string& id : deleted) ops.push_back(Json{{"op", "delete_line"}, {"id", id}});
    std::map<std::string, const core::StoryLine*> by_id;
    for (const core::StoryLine& line : book.story) by_id.emplace(line.id, &line);
    for (const ScriptRow& r : all) {
        const auto found = r.id ? by_id.find(*r.id) : by_id.end();
        if (found == by_id.end()) continue;
        const core::StoryLine& line = *found->second;
        const lettering::Marks marks = lettering::parse_marks(r.text);
        const QString was_balloon = line.balloon.empty() ? QStringLiteral("speech") : qs(line.balloon);
        if (lettering::same_marks(marks, line) && r.speaker == qs(line.speaker) && r.balloon == was_balloon) continue;
        ops.push_back(Json{{"op", "edit_line"},
                           {"id", line.id},
                           {"text", marks.text},
                           {"speaker", utf8(r.speaker)},
                           {"balloon", utf8(r.balloon)},
                           {"ruby_runs", marks.ruby_runs},
                           {"emphasis_runs", marks.emphasis_runs},
                           {"style_runs", marks.style_runs}});
        lettering::put_ruby(ops.back(), marks, &line, true);  // (TEXT-01: by place when Python's runs would not say where)
        if (marks.text != line.text || r.balloon != was_balloon) {
            const core::Frame* frame = nullptr;
            for (const auto& page : book.pages) {
                if (page->index == line.page_index) {
                    if (line.frame_id) frame = page->find_frame(*line.frame_id);
                    break;
                }
            }
            Json op{{"op", "move_line"}, {"id", line.id}};
            const Json size = lettering::refit(line, frame, marks.text, utf8(r.balloon), line.wrap == "vertical");
            for (const auto& [key, value] : size.items()) op[key] = value;
            ops.push_back(op);
        }
    }
    // new rows: placed on a working copy, panel by panel in reading order
    const core::CommandBus bus(render::ops_registry());
    const core::Actor actor("genko");
    core::Document work = book;
    work.strict_gates = false;
    work = bus.apply(work, ops, actor).doc;
    std::vector<std::pair<std::int64_t, std::vector<std::size_t>>> new_by_page;  // (in the order the pages first come)
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (all[i].id) continue;
        all[i].id = core::new_id();
        auto it = std::find_if(new_by_page.begin(), new_by_page.end(), [&](const auto& p) { return p.first == all[i].page; });
        if (it == new_by_page.end()) {
            new_by_page.emplace_back(all[i].page, std::vector<std::size_t>{});
            it = std::prev(new_by_page.end());
        }
        it->second.push_back(i);
    }
    for (const auto& [page_no, fresh] : new_by_page) {
        std::size_t page_at = work.pages.size();
        for (std::size_t p = 0; p < work.pages.size(); ++p) {
            if (work.pages[p]->index == core::Num(page_no)) {
                page_at = p;
                break;
            }
        }
        if (page_at == work.pages.size()) throw core::Error("value", "no page " + std::to_string(page_no));
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            const ScriptRow& r = all[fresh[i]];
            const core::Page& page = work.page(page_at);
            const auto frames = page.leaf_frames();
            const core::Frame* frame = frames.empty() ? nullptr : frames[i * frames.size() / fresh.size()];
            const lettering::Marks marks = lettering::parse_marks(r.text);
            Json op{{"op", "add_line"}, {"page", page_no}, {"id", *r.id}, {"text", marks.text}, {"speaker", utf8(r.speaker)}, {"balloon", utf8(r.balloon)}};
            lettering::put_ruby(op, marks, nullptr, false);
            if (!marks.emphasis_runs.empty()) op["emphasis_runs"] = marks.emphasis_runs;
            if (!marks.style_runs.empty()) op["style_runs"] = marks.style_runs;
            if (frame != nullptr) {
                op["frame_id"] = frame->id;
                const Json box = lettering::place_new(work, page, *frame, marks.text, utf8(r.balloon), true);
                for (const auto& [key, value] : box.items()) op[key] = value;
            }
            work = bus.apply(work, Json::array({op}), actor).doc;
            ops.push_back(op);
        }
    }
    // the table's order is each page's reading order
    for (const auto& page : work.pages) {
        std::vector<std::string> order;
        for (const ScriptRow& r : all) {
            if (core::Num(r.page) == page->index) order.push_back(*r.id);
        }
        std::vector<std::string> current;
        for (const core::StoryLine* line : work.story_for_page(page->index)) current.push_back(line->id);
        std::vector<std::string> a = order;
        std::vector<std::string> b = current;
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
        if (!order.empty() && a == b && order != current) {
            Json ids = Json::array();
            for (const std::string& id : order) ids.push_back(id);
            ops.push_back(Json{{"op", "reorder_lines"}, {"page", page->index.json()}, {"order", ids}});
        }
    }
    return ops;
}

void StoryEditor::apply() {
    const auto all = rows();
    const auto empty = std::count_if(all.begin(), all.end(), [](const ScriptRow& r) { return r.text.isEmpty(); });
    if (empty > 0) status->setText(QStringLiteral("空の行が %1 行あります（反映しません）").arg(empty));
    Json ops;
    try {
        ops = plan();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("反映できませんでした（%1）").arg(QString::fromUtf8(error.what())));
        return;
    }
    if (ops.empty()) {
        status->setText(QStringLiteral("変わったところはありません"));
        return;
    }
    if (window_->apply_ops(ops)) {
        load();
        status->setText(QStringLiteral("原稿に反映しました（%1 件の変更。元に戻す で一度に戻せます）").arg(ops.size()));
    }
}

// --- 検索と置換 (台詞の検索・置換 before PC5 G-d) ---------------------------------------------------------------------------

namespace {

// re.escape(text) as Python 3.12 escapes (core/ops_lines' replace_text reads the words so): its special characters.
std::string escaped(const std::string& text) {
    static constexpr std::string_view kSpecial = "()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
    std::string out;
    for (const char ch : text) {
        if (kSpecial.find(ch) != std::string_view::npos) out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

}  // namespace

ReplaceDialog::ReplaceDialog(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("replace_dialog"));
    setWindowTitle(QStringLiteral("検索と置換"));  // (PC5 G-d: Photoshop's 編集 → 検索と置換…)
    resize(520, 480);
    find = new QLineEdit;
    find->setPlaceholderText(QStringLiteral("探す言葉"));
    replace = new QLineEdit;
    replace->setPlaceholderText(QStringLiteral("置き換える言葉（空にすると消す）"));
    regex = new QCheckBox(QStringLiteral("正規表現で探す"));
    speakers = new QCheckBox(QStringLiteral("話者の名前も"));
    // (PC5 G-d: Photoshop's options — 大文字と小文字を区別 on, as Genko's search was —, and where the last was found)
    case_sensitive = new QCheckBox(QStringLiteral("大文字と小文字を区別"));
    case_sensitive->setChecked(true);
    whole_word = new QCheckBox(QStringLiteral("単語単位"));
    backward = new QCheckBox(QStringLiteral("前方へ検索"));
    all_layers = new QCheckBox(QStringLiteral("すべてのレイヤーを検索"));
    all_layers->setChecked(true);
    all_layers->setToolTip(QStringLiteral("オフ: 選んだテキストレイヤー・台詞だけを探します"));
    found = new QLabel;
    found->setWordWrap(true);
    for (QCheckBox* box : {regex, case_sensitive, whole_word, backward, all_layers}) connect(box, &QCheckBox::toggled, this, [this] { last_.reset(); });
    results = new QListWidget;
    connect(results, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { go(item); });
    count = new QLabel;
    auto* search_button = new QPushButton(QStringLiteral("探す"));
    connect(search_button, &QPushButton::clicked, this, [this] { search(); });
    auto* run = new QPushButton(QStringLiteral("すべてを置換"));
    connect(run, &QPushButton::clicked, this, [this] { replace_all(); });
    auto* next = new QPushButton(QStringLiteral("次を検索"));
    next->setObjectName(QStringLiteral("find_next"));
    connect(next, &QPushButton::clicked, this, [this] { find_next(); });
    auto* one = new QPushButton(QStringLiteral("置換"));
    one->setObjectName(QStringLiteral("replace_one"));
    connect(one, &QPushButton::clicked, this, [this] { replace_one(); });
    auto* both = new QPushButton(QStringLiteral("置換して次を検索"));
    both->setObjectName(QStringLiteral("replace_and_find"));
    connect(both, &QPushButton::clicked, this, [this] { replace_and_find(); });
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("探す"), find);
    form->addRow(QStringLiteral("置き換え"), replace);
    auto* options = new QGridLayout;
    options->addWidget(all_layers, 0, 0);
    options->addWidget(backward, 0, 1);
    options->addWidget(case_sensitive, 1, 0);
    options->addWidget(whole_word, 1, 1);
    options->addWidget(regex, 2, 0);
    options->addWidget(speakers, 2, 1);
    auto* steps = new QHBoxLayout;
    for (QPushButton* b : {next, one, both}) steps->addWidget(b);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(search_button);
    buttons->addWidget(run);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(options);
    layout->addLayout(steps);
    layout->addWidget(found);
    layout->addLayout(buttons);
    layout->addWidget(count);
    layout->addWidget(results, 1);
    connect(find, &QLineEdit::returnPressed, this, [this] { search(); });
}

int ReplaceDialog::search() {
    results->clear();
    const QString text = find->text();
    if (text.isEmpty()) return 0;
    // re.compile(text if regex else re.escape(text)), and len(pattern.findall(…)) of each line (core/pyre: Python's re);
    // the search bounded as replace_text's is (a pattern that would take too long is said, never left running)
    // (PC5 G-d: 単語単位 and 大文字と小文字を区別 as replace_text reads them)
    std::optional<core::PyPattern> compiled;
    std::optional<core::PyLiteralPattern> literal;
    try {
        if (regex->isChecked() || whole_word->isChecked() || !case_sensitive->isChecked()) {
            const std::string body = regex->isChecked() ? utf8(text) : escaped(utf8(text));
            compiled.emplace(whole_word->isChecked() ? "(?<!\\w)(?:" + body + ")(?!\\w)" : body, case_sensitive->isChecked() ? 0 : core::PyPattern::kIgnoreCase);
        } else {
            literal.emplace(utf8(text), false);
        }
    } catch (const std::exception&) {  // (re.error; and what Python's dialog would not catch either)
        count->setText(QStringLiteral("正規表現が読めません"));
        return 0;
    }
    core::PyReBudget budget;
    const auto hits_in = [&](const std::string& words) -> int {
        if (literal) return static_cast<int>(literal->subn("", words).second);
        return static_cast<int>(compiled->count(words, budget));
    };
    int found = 0;
    for (const core::StoryLine& line : window_->book().story) {
        int hits = 0;
        try {
            hits = hits_in(line.text) + (speakers->isChecked() ? hits_in(line.speaker) : 0);
        } catch (const std::exception& error) {
            // (core::Error "regex_too_slow"; or what Python's findall raises, as its SystemError for a group's span)
            const auto* own = dynamic_cast<const core::Error*>(&error);
            results->clear();
            count->setText(own != nullptr && own->code() == "regex_too_slow"
                               ? QStringLiteral("この正規表現は探すのに時間がかかりすぎるため、探すのをやめました（書き方を見直してください）")
                               : QStringLiteral("この正規表現では探せませんでした（%1）").arg(QString::fromUtf8(error.what())));
            return 0;
        }
        if (hits == 0) continue;
        found += hits;
        const QString who = line.speaker.empty() ? QString() : qs(line.speaker) + QStringLiteral("：");
        const QString start = qs(render::text::utf8(render::text::u32(line.text).substr(0, 40)));
        auto* item = new QListWidgetItem(QStringLiteral("%1 ページ　%2%3").arg(qs(line.page_index.repr()), who, start));
        item->setData(Qt::UserRole, QStringList{qs(line.page_index.json().dump()), qs(line.id)});
        results->addItem(item);
    }
    // (PC5 G-d: the type layers replace_text replaces too — the unlocked ones of the pages shown, top first)
    const core::Document& book = window_->book();
    for (std::size_t row = 0; row < book.pages.size(); ++row) {
        if (book.is_deferred(row)) continue;
        const core::Page& page = book.page(row);
        for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {
            if (!core::type_text::is_type_layer(*it) || !it->type || it->locked) continue;
            int hits = 0;
            try {
                hits = hits_in(it->type->text);
            } catch (const std::exception&) {
                results->clear();
                count->setText(QStringLiteral("この正規表現は探すのに時間がかかりすぎるため、探すのをやめました（書き方を見直してください）"));
                return 0;
            }
            if (hits == 0) continue;
            found += hits;
            const QString start = qs(render::text::utf8(render::text::u32(it->type->text).substr(0, 40)));
            auto* item = new QListWidgetItem(QStringLiteral("%1 ページ　テキストレイヤー：%2").arg(qs(page.index.repr()), start));
            item->setData(Qt::UserRole, QStringList{qs(page.index.json().dump()), QString(), qs(it->id)});
            results->addItem(item);
        }
    }
    count->setText(found != 0 ? QStringLiteral("%1 か所（%2 行）").arg(found).arg(results->count()) : QStringLiteral("見つかりません"));
    return found;
}

void ReplaceDialog::go(QListWidgetItem* item) {
    const QStringList data = item->data(Qt::UserRole).toStringList();
    if (data.size() != 2 && data.size() != 3) return;
    const Json index = Json::parse(data[0].toStdString(), nullptr, false);
    const auto& pages = window_->book().pages;
    for (std::size_t row = 0; row < pages.size(); ++row) {
        if (pages[row]->index.json() == index) {
            window_->select_page(static_cast<int>(row));
            if (data.size() == 3) {  // (a type layer's: chosen)
                window_->set_target_layer(data[2].toStdString());
                return;
            }
            window_->on_line_selected(data[1].toStdString(), true);
            return;
        }
    }
}

void ReplaceDialog::replace_all() {
    if (!all_layers->isChecked()) {  // (PC5 G-d: the chosen text alone — its words replaced in one edit_type)
        const std::optional<Json> target = window_->type_target(true);
        const core::StoryLine* record = target ? window_->type_record(*target) : nullptr;
        if (record == nullptr || find->text().isEmpty()) return;
        std::pair<std::string, std::int64_t> out;
        try {
            out = replaced(record->text);  // (re.subn over the whole text, as replace_text: review P3-1)
        } catch (const std::exception& error) {
            found->setText(QStringLiteral("探せませんでした（%1）").arg(QString::fromUtf8(error.what())));
            return;
        }
        if (out.second == 0 || out.first == record->text) {
            found->setText(out.second == 0 ? QStringLiteral("見つかりません") : QStringLiteral("置き換えても同じです"));
            return;
        }
        Json op{{"op", "edit_type"}, {"text", out.first}};
        for (const auto& [key, value] : target->items()) op[key] = value;
        if (window_->apply_ops(Json::array({op}))) window_->flash(QStringLiteral("%1 か所を置き換えました（元に戻すは 1 回）").arg(out.second), 4000);
        last_.reset();
        return;
    }
    const int hits = search();
    if (hits == 0) return;
    Json op{{"op", "replace_text"},
            {"find", utf8(find->text())},
            {"replace", utf8(replace->text())},
            {"regex", regex->isChecked()},
            {"speakers", speakers->isChecked()}};
    // (PC5 G-d: what replace_text takes besides — written only when they differ from its defaults, as before)
    if (!case_sensitive->isChecked()) op["case"] = false;
    if (whole_word->isChecked()) op["whole_word"] = true;
    if (window_->apply_ops(Json::array({op}))) {
        window_->flash(QStringLiteral("%1 か所を置き換えました（元に戻すは 1 回）").arg(hits), 4000);
        search();
    }
    last_.reset();
}

std::vector<ReplaceDialog::Spot> ReplaceDialog::spots() const {
    std::vector<Spot> out;
    const core::Document& book = window_->book();
    if (!all_layers->isChecked()) {
        const std::optional<Json> target = window_->type_target(false);
        const core::StoryLine* record = target ? window_->type_record(*target) : nullptr;
        if (record != nullptr) out.push_back(Spot{*target, -1, record->text});
        return out;
    }
    for (std::size_t row = 0; row < book.pages.size(); ++row) {
        if (book.is_deferred(row)) continue;
        const core::Page& page = book.page(row);
        for (const core::StoryLine* line : book.story_for_page(page.index)) out.push_back(Spot{Json{{"line", line->id}}, static_cast<int>(row), line->text});
        for (auto it = page.layers.rbegin(); it != page.layers.rend(); ++it) {
            if (!core::type_text::is_type_layer(*it) || !it->type || it->locked) continue;
            out.push_back(Spot{Json{{"page", page.index.json()}, {"layer_id", it->id}}, static_cast<int>(row), it->type->text});
        }
    }
    return out;
}

std::vector<std::pair<std::int64_t, std::int64_t>> ReplaceDialog::places(const std::string& text) const {
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    const std::string words = utf8(find->text());
    if (words.empty()) return out;
    const std::string body = regex->isChecked() ? words : escaped(words);
    const core::PyPattern pattern(whole_word->isChecked() ? "(?<!\\w)(?:" + body + ")(?!\\w)" : body, case_sensitive->isChecked() ? 0 : core::PyPattern::kIgnoreCase);
    core::PyReBudget budget;
    for (const auto& groups : pattern.spans(text, budget)) {
        if (!groups.empty() && groups.front().second > groups.front().first) out.push_back(groups.front());
    }
    return out;
}

std::string ReplaceDialog::replacement(const std::string& text, std::int64_t start, std::int64_t end) const {
    // (the words found as replace_text replaces them: its template read as re.sub reads it, the match in its text)
    const std::string words = utf8(find->text());
    const std::string body = regex->isChecked() ? words : escaped(words);
    const core::PyPattern pattern(whole_word->isChecked() ? "(?<!\\w)(?:" + body + ")(?!\\w)" : body, case_sensitive->isChecked() ? 0 : core::PyPattern::kIgnoreCase);
    core::PyReBudget budget;
    std::vector<core::PyReMatch> matches;
    (void)pattern.subn(utf8(replace->text()), text, &matches, budget);
    for (const core::PyReMatch& m : matches) {
        if (static_cast<std::int64_t>(m.start) == start && static_cast<std::int64_t>(m.end) == end) return core::ruby::to_utf8(m.with);
    }
    throw core::Error("not_found", "the words found are not where they were");
}

std::pair<std::string, std::int64_t> ReplaceDialog::replaced(const std::string& text) const {
    const std::string words = utf8(find->text());
    const std::string body = regex->isChecked() ? words : escaped(words);
    const core::PyPattern pattern(whole_word->isChecked() ? "(?<!\\w)(?:" + body + ")(?!\\w)" : body, case_sensitive->isChecked() ? 0 : core::PyPattern::kIgnoreCase);
    core::PyReBudget budget;
    return pattern.subn(utf8(replace->text()), text, nullptr, budget);
}

bool ReplaceDialog::find_next() {
    if (find->text().isEmpty()) return false;
    const std::vector<Spot> all = spots();
    if (all.empty()) {
        found->setText(QStringLiteral("探すテキストがありません"));
        return false;
    }
    // where the last was found (its text's place in the order; none: the start — the end, backward)
    const bool back = backward->isChecked();
    std::size_t from_spot = back ? all.size() - 1 : 0;
    std::optional<std::int64_t> from_at;
    if (last_) {
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (all[i].target == last_->target) {
                from_spot = i;
                from_at = back ? last_->start : last_->end;
            }
        }
    }
    try {
        for (std::size_t step = 0; step <= all.size(); ++step) {
            const std::size_t i = back ? (from_spot + all.size() * 2 - step) % all.size() : (from_spot + step) % all.size();
            const Spot& spot = all[i];
            std::vector<std::pair<std::int64_t, std::int64_t>> at = places(spot.text);
            if (back) std::reverse(at.begin(), at.end());
            for (const auto& [s, e] : at) {
                if (step == 0 && from_at && (back ? e > *from_at : s < *from_at)) continue;
                if (step == all.size() && from_at && (back ? e <= *from_at : s >= *from_at)) continue;  // (round again: up to where it began)
                last_ = Found{spot.target, spot.text, s, e};
                if (spot.page_row >= 0) window_->select_page(spot.page_row);
                if (spot.target.contains("line")) {
                    window_->on_line_selected(spot.target["line"].get<std::string>(), false);
                } else {
                    window_->set_target_layer(spot.target["layer_id"].get<std::string>());
                }
                const std::u32string cps = core::ruby::code_points(spot.text);
                const QString word = QString::fromStdString(core::ruby::to_utf8(std::u32string_view(cps).substr(static_cast<std::size_t>(s), static_cast<std::size_t>(e - s))));
                found->setText(QStringLiteral("「%1」が見つかりました%2").arg(word, step == all.size() || (step > 0 && i == (back ? all.size() - 1 : 0) && from_spot != i)
                                                                                      ? QStringLiteral("（はじめから探しました）")
                                                                                      : QString()));
                window_->refresh_type_panels();
                return true;
            }
        }
    } catch (const std::exception& error) {
        const auto* own = dynamic_cast<const core::Error*>(&error);
        found->setText(own != nullptr && own->code() == "regex_too_slow" ? QStringLiteral("この正規表現は探すのに時間がかかりすぎるため、探すのをやめました")
                                                                         : QStringLiteral("探せませんでした（%1）").arg(QString::fromUtf8(error.what())));
        return false;
    }
    last_.reset();
    found->setText(QStringLiteral("見つかりません"));
    return false;
}

bool ReplaceDialog::replace_one() {
    if (!last_) return find_next() && false;
    // (the place found, while its text is as it was)
    const std::vector<Spot> all = spots();
    const auto it = std::find_if(all.begin(), all.end(), [&](const Spot& s) { return s.target == last_->target; });
    if (it == all.end() || it->text != last_->text) {
        last_.reset();
        found->setText(QStringLiteral("見つけた所が変わりました。もう一度探します"));
        return false;
    }
    const std::u32string cps = core::ruby::code_points(it->text);
    std::string with;
    try {
        with = replacement(it->text, last_->start, last_->end);
    } catch (const std::exception& error) {
        found->setText(QStringLiteral("置き換えられませんでした（%1）").arg(QString::fromUtf8(error.what())));
        return false;
    }
    // (graphemes, as edit_type takes them: a place inside a grapheme takes the whole of it)
    const std::vector<std::size_t> b = core::ruby::bounds(cps);
    const auto g_start = static_cast<std::int64_t>(std::upper_bound(b.begin(), b.end(), static_cast<std::size_t>(last_->start)) - b.begin()) - 1;
    const auto g_end = static_cast<std::int64_t>(std::lower_bound(b.begin(), b.end(), static_cast<std::size_t>(last_->end)) - b.begin());
    Json op{{"op", "edit_type"}, {"replace", Json{{"start", g_start}, {"end", g_end}, {"text", with}}}};
    for (const auto& [key, value] : last_->target.items()) op[key] = value;
    if (!window_->apply_ops(Json::array({op}))) return false;
    // (searched on from after what it put)
    const std::int64_t end = last_->start + static_cast<std::int64_t>(core::ruby::code_points(with).size());
    std::string now = it->text;
    for (const Spot& s : spots()) {
        if (s.target == last_->target) now = s.text;
    }
    last_ = Found{last_->target, now, backward->isChecked() ? last_->start : end, backward->isChecked() ? last_->start : end};
    found->setText(QStringLiteral("置き換えました"));
    window_->refresh_type_panels();
    return true;
}

void ReplaceDialog::replace_and_find() {
    if (replace_one()) find_next();
}

}  // namespace genko::app
