#include "app/story_editor.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
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
#include <set>
#include <tuple>

#include "app/ask.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "app/wording.hpp"
#include "core/actor.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "render/ops_registry.hpp"
#include "render/text/fonts.hpp"

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
            if (!marks.ruby_runs.empty()) op["ruby_runs"] = marks.ruby_runs;
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

// --- 台詞の検索・置換 ---------------------------------------------------------------------------------------------------

ReplaceDialog::ReplaceDialog(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("replace_dialog"));
    setWindowTitle(QStringLiteral("台詞の検索・置換"));
    resize(520, 480);
    find = new QLineEdit;
    find->setPlaceholderText(QStringLiteral("探す言葉"));
    replace = new QLineEdit;
    replace->setPlaceholderText(QStringLiteral("置き換える言葉（空にすると消す）"));
    regex = new QCheckBox(QStringLiteral("正規表現で探す"));
    speakers = new QCheckBox(QStringLiteral("話者の名前も"));
    results = new QListWidget;
    connect(results, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { go(item); });
    count = new QLabel;
    auto* search_button = new QPushButton(QStringLiteral("探す"));
    connect(search_button, &QPushButton::clicked, this, [this] { search(); });
    auto* run = new QPushButton(QStringLiteral("全部置き換える"));
    connect(run, &QPushButton::clicked, this, [this] { replace_all(); });
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("探す"), find);
    form->addRow(QStringLiteral("置き換え"), replace);
    auto* options = new QHBoxLayout;
    options->addWidget(regex);
    options->addWidget(speakers);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(search_button);
    buttons->addWidget(run);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addLayout(options);
    layout->addLayout(buttons);
    layout->addWidget(count);
    layout->addWidget(results, 1);
    connect(find, &QLineEdit::returnPressed, this, [this] { search(); });
}

int ReplaceDialog::search() {
    results->clear();
    const QString text = find->text();
    if (text.isEmpty()) return 0;
    const QRegularExpression re(regex->isChecked() ? text : QRegularExpression::escape(text), QRegularExpression::UseUnicodePropertiesOption);
    if (!re.isValid()) {
        count->setText(QStringLiteral("正規表現が読めません"));
        return 0;
    }
    const auto hits_in = [&re](const std::string& words) {
        int n = 0;
        auto it = re.globalMatch(qs(words));
        while (it.hasNext()) {
            it.next();
            ++n;
        }
        return n;
    };
    int found = 0;
    for (const core::StoryLine& line : window_->book().story) {
        const int hits = hits_in(line.text) + (speakers->isChecked() ? hits_in(line.speaker) : 0);
        if (hits == 0) continue;
        found += hits;
        const QString who = line.speaker.empty() ? QString() : qs(line.speaker) + QStringLiteral("：");
        const QString start = qs(render::text::utf8(render::text::u32(line.text).substr(0, 40)));
        auto* item = new QListWidgetItem(QStringLiteral("%1 ページ　%2%3").arg(qs(line.page_index.repr()), who, start));
        item->setData(Qt::UserRole, QStringList{qs(line.page_index.json().dump()), qs(line.id)});
        results->addItem(item);
    }
    count->setText(found != 0 ? QStringLiteral("%1 か所（%2 行）").arg(found).arg(results->count()) : QStringLiteral("見つかりません"));
    return found;
}

void ReplaceDialog::go(QListWidgetItem* item) {
    const QStringList data = item->data(Qt::UserRole).toStringList();
    if (data.size() != 2) return;
    const Json index = Json::parse(data[0].toStdString(), nullptr, false);
    const auto& pages = window_->book().pages;
    for (std::size_t row = 0; row < pages.size(); ++row) {
        if (pages[row]->index.json() == index) {
            window_->select_page(static_cast<int>(row));
            window_->on_line_selected(data[1].toStdString(), true);
            return;
        }
    }
}

void ReplaceDialog::replace_all() {
    const int found = search();
    if (found == 0) return;
    const Json op{{"op", "replace_text"},
                  {"find", utf8(find->text())},
                  {"replace", utf8(replace->text())},
                  {"regex", regex->isChecked()},
                  {"speakers", speakers->isChecked()}};
    if (window_->apply_ops(Json::array({op}))) {
        window_->flash(QStringLiteral("%1 か所を置き換えました（元に戻すは 1 回）").arg(found), 4000);
        search();
    }
}

}  // namespace genko::app
