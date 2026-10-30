// PC5 G-d: スペルチェック…, 表記の表… and 見つからないフォントを解決… (app/type_dialogs.hpp).

#include "app/type_dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/lettering.hpp"
#include "app/main_window.hpp"
#include "core/prefs_table.hpp"
#include "core/ruby.hpp"
#include "service/briefs.hpp"

namespace genko::app {

using core::Json;

namespace {

QString qs(const std::string& s) { return QString::fromStdString(s); }

std::string lowered(const std::string& word) { return QString::fromStdString(word).toLower().toStdString(); }

// This computer's user dictionary and notation table.
std::set<std::string> user_dictionary() {
    const auto store = settings();
    const Json words = core::prefs::value(*store, "type.userDictionary");
    return service::spell::user_words(words.is_string() ? words.get<std::string>() : std::string());
}

std::vector<service::spell::Notation> notation_table() {
    const auto store = settings();
    const Json table = core::prefs::value(*store, "type.notations");
    return service::spell::notations_of(table.is_string() ? Json::parse(table.get<std::string>(), nullptr, false) : Json());
}

// The text a finding is in, as the book holds it now (none: gone).
const core::StoryLine* text_of(const MainWindow* window, const service::spell::Finding& f) {
    const core::Document& book = window->book();
    if (!f.line.empty()) {
        for (const core::StoryLine& line : book.story) {
            if (line.id == f.line) return &line;
        }
        return nullptr;
    }
    for (std::size_t i = 0; i < book.pages.size(); ++i) {
        if (book.is_deferred(i) || !(book.page(i).index == f.page)) continue;
        for (const core::Layer& layer : book.page(i).layers) {
            if (layer.id == f.layer_id && layer.type) return layer.type.get();
        }
    }
    return nullptr;
}

Json target_of(const service::spell::Finding& f) {
    if (!f.line.empty()) return Json{{"line", f.line}};
    return Json{{"page", f.page.json()}, {"layer_id", f.layer_id}};
}

bool same_text(const service::spell::Finding& a, const service::spell::Finding& b) { return a.line == b.line && a.layer_id == b.layer_id && a.page == b.page; }

}  // namespace

// --- スペルチェック ------------------------------------------------------------------------------------------------------

SpellingDialog::SpellingDialog(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("spelling_dialog"));
    setWindowTitle(QStringLiteral("スペルチェック"));
    resize(480, 400);
    word = new QLineEdit;
    word->setReadOnly(true);
    word->setObjectName(QStringLiteral("spelling_word"));
    change_to = new QLineEdit;
    change_to->setObjectName(QStringLiteral("spelling_change_to"));
    suggestions = new QListWidget;
    suggestions->setObjectName(QStringLiteral("spelling_suggestions"));
    connect(suggestions, &QListWidget::currentTextChanged, this, [this](const QString& text) {
        if (!text.isEmpty()) change_to->setText(text);
    });
    connect(suggestions, &QListWidget::itemDoubleClicked, this, [this] { change(); });
    where = new QLabel;
    where->setWordWrap(true);
    all_layers = new QCheckBox(QStringLiteral("すべてのレイヤーを確認"));
    all_layers->setChecked(true);
    all_layers->setToolTip(QStringLiteral("オフ: 表示しているページだけを確かめます"));
    sfx = new QCheckBox(QStringLiteral("効果音（描き文字）も確認"));
    sfx->setToolTip(QStringLiteral("オフ: 効果音のフキダシの台詞は見ません（ローマ字の擬音を誤りと言わないため）"));
    for (QCheckBox* box : {all_layers, sfx}) connect(box, &QCheckBox::toggled, this, [this] { check(); });
    const auto button = [this](const QString& text, const QString& name) {
        auto* b = new QPushButton(text);
        b->setObjectName(name);
        (void)this;
        return b;
    };
    ignore_button = button(QStringLiteral("無視"), QStringLiteral("spelling_ignore"));
    ignore_all_button = button(QStringLiteral("すべてを無視"), QStringLiteral("spelling_ignore_all"));
    change_button = button(QStringLiteral("変更"), QStringLiteral("spelling_change"));
    change_all_button = button(QStringLiteral("すべてを変更"), QStringLiteral("spelling_change_all"));
    add_button = button(QStringLiteral("辞書に追加"), QStringLiteral("spelling_add"));
    table_button = button(QStringLiteral("表記の表…"), QStringLiteral("spelling_table"));
    auto* done = button(QStringLiteral("完了"), QStringLiteral("spelling_done"));
    connect(ignore_button, &QPushButton::clicked, this, [this] { ignore(); });
    connect(ignore_all_button, &QPushButton::clicked, this, [this] { ignore_all(); });
    connect(change_button, &QPushButton::clicked, this, [this] { change(); });
    connect(change_all_button, &QPushButton::clicked, this, [this] { change_all(); });
    connect(add_button, &QPushButton::clicked, this, [this] { add_to_dictionary(); });
    connect(table_button, &QPushButton::clicked, this, [this] {
        NotationDialog table(this);
        if (window_ != nullptr && ask::exec(&table) == QDialog::Accepted) {
            table.save();
            check();
        }
    });
    connect(done, &QPushButton::clicked, this, &QDialog::accept);
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("辞書にない語"), word);
    form->addRow(QStringLiteral("変更後の文字列"), change_to);
    form->addRow(QStringLiteral("候補"), suggestions);
    auto* side = new QVBoxLayout;
    for (QPushButton* b : {done, ignore_button, ignore_all_button, change_button, change_all_button, add_button, table_button}) side->addWidget(b);
    side->addStretch(1);
    auto* top = new QHBoxLayout;
    top->addLayout(form, 1);
    top->addLayout(side);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(where);
    layout->addLayout(top, 1);
    layout->addWidget(all_layers);
    layout->addWidget(sfx);
    check();
}

void SpellingDialog::check() {
    service::spell::Options options;
    options.user = user_dictionary();
    options.ignore = ignored_;
    options.notations = notation_table();
    options.sfx = sfx->isChecked();
    if (!all_layers->isChecked()) {
        if (const core::Page* page = window_->current_page()) options.page = page->index;
    }
    try {
        findings_ = service::spell::check(window_->book(), options);
    } catch (const std::exception& error) {
        findings_.clear();
        where->setText(QStringLiteral("確かめられませんでした（%1）").arg(QString::fromUtf8(error.what())));
    }
    // (a notation the person chose to ignore: left out too)
    std::erase_if(findings_, [this](const service::spell::Finding& f) { return f.kind == "notation" && ignored_.contains(lowered(f.word)); });
    at_ = 0;
    show_current();
}

const service::spell::Finding* SpellingDialog::current() const { return at_ < findings_.size() ? &findings_[at_] : nullptr; }

void SpellingDialog::show_current() {
    const service::spell::Finding* f = current();
    suggestions->clear();
    for (QPushButton* b : {ignore_button, ignore_all_button, change_button, change_all_button, add_button}) b->setEnabled(f != nullptr);
    if (f == nullptr) {
        word->clear();
        change_to->clear();
        where->setText(findings_.empty() ? QStringLiteral("辞書にない語・表記の揺れは見つかりませんでした") : QStringLiteral("スペルチェックが終わりました"));
        return;
    }
    word->setText(qs(f->word));
    add_button->setEnabled(f->kind == "spelling");
    for (const std::string& s : f->suggestions) suggestions->addItem(qs(s));
    change_to->setText(f->suggestions.empty() ? qs(f->word) : qs(f->suggestions.front()));
    if (!f->suggestions.empty()) suggestions->setCurrentRow(0);
    where->setText(QStringLiteral("%1 ページの%2（%3〜%4 字目）%5")
                       .arg(qs(f->page.repr()), f->line.empty() ? QStringLiteral("テキストレイヤー") : QStringLiteral("台詞"))
                       .arg(f->start + 1)
                       .arg(f->end)
                       .arg(f->kind == "notation" ? QStringLiteral("　表記の表: 「%1」→「%2」").arg(qs(f->word), qs(f->suggestions.front())) : QString()));
    // (the page shown, the text chosen)
    const core::Document& book = window_->book();
    for (std::size_t row = 0; row < book.pages.size(); ++row) {
        if (book.pages[row]->index == f->page) {
            window_->select_page(static_cast<int>(row));
            break;
        }
    }
    if (!f->line.empty()) window_->on_line_selected(f->line, false);
    else window_->set_target_layer(f->layer_id);
}

void SpellingDialog::step() {
    ++at_;
    show_current();
}

void SpellingDialog::ignore() { step(); }

void SpellingDialog::ignore_all() {
    const service::spell::Finding* f = current();
    if (f == nullptr) return;
    const std::string low = lowered(f->word);
    ignored_.insert(low);
    const std::string kind = f->kind;
    // (every later finding of the word: gone for this check)
    findings_.erase(std::remove_if(findings_.begin() + static_cast<std::ptrdiff_t>(at_), findings_.end(),
                                   [&](const service::spell::Finding& g) { return g.kind == kind && lowered(g.word) == low; }),
                    findings_.end());
    show_current();
}

void SpellingDialog::change() {
    const service::spell::Finding* f = current();
    if (f == nullptr) return;
    const std::string with = change_to->text().toStdString();
    const core::StoryLine* record = text_of(window_, *f);
    // (the word still where it was found)
    if (record == nullptr || core::ruby::slice(record->text, f->start, f->end) != f->word) {
        window_->flash(QStringLiteral("文字が変わったので確かめ直します"), 3000);
        check();
        return;
    }
    Json op{{"op", "edit_type"}, {"replace", Json{{"start", f->start}, {"end", f->end}, {"text", with}}}};
    const Json named = target_of(*f);
    for (const auto& [key, value] : named.items()) op[key] = value;
    if (!window_->apply_ops(Json::array({op}))) return;
    // (the later findings of the same text moved by what changed)
    const std::int64_t delta = core::ruby::count(with) - (f->end - f->start);
    const service::spell::Finding changed = *f;
    for (std::size_t i = at_ + 1; i < findings_.size(); ++i) {
        if (same_text(findings_[i], changed) && findings_[i].start >= changed.end) {
            findings_[i].start += delta;
            findings_[i].end += delta;
        }
    }
    step();
}

void SpellingDialog::change_all() {
    const service::spell::Finding* f = current();
    if (f == nullptr) return;
    const std::string with = change_to->text().toStdString();
    const std::string from = f->word;
    const std::string kind = f->kind;
    // every finding of the word (each text's from its end: the places before stay where they are), in one step
    std::vector<service::spell::Finding> all;
    for (std::size_t i = at_; i < findings_.size(); ++i) {
        if (findings_[i].kind == kind && findings_[i].word == from) all.push_back(findings_[i]);
    }
    const auto text_key = [](const service::spell::Finding& g) { return g.line + "\n" + g.layer_id + "\n" + g.page.repr(); };
    std::stable_sort(all.begin(), all.end(), [&](const service::spell::Finding& a, const service::spell::Finding& b) {
        const std::string ka = text_key(a), kb = text_key(b);
        if (ka != kb) return ka < kb;
        return a.start > b.start;
    });
    Json ops = Json::array();
    for (const service::spell::Finding& g : all) {
        const core::StoryLine* record = text_of(window_, g);
        if (record == nullptr || core::ruby::slice(record->text, g.start, g.end) != g.word) continue;
        Json op{{"op", "edit_type"}, {"replace", Json{{"start", g.start}, {"end", g.end}, {"text", with}}}};
        const Json named = target_of(g);
        for (const auto& [key, value] : named.items()) op[key] = value;
        ops.push_back(std::move(op));
    }
    if (ops.empty() || !window_->apply_ops(ops)) return;
    window_->flash(QStringLiteral("%1 か所を変更しました（元に戻すは 1 回）").arg(ops.size()), 4000);
    // (what is left: each later finding moved by the changes before it in its text)
    const std::int64_t grown = core::ruby::count(with) - core::ruby::count(from);
    std::vector<service::spell::Finding> left;
    for (std::size_t i = at_; i < findings_.size(); ++i) {
        const service::spell::Finding& g = findings_[i];
        if (g.kind == kind && g.word == from) continue;
        service::spell::Finding moved = g;
        for (const service::spell::Finding& c : all) {
            if (same_text(c, g) && c.end <= g.start) {
                moved.start += grown;
                moved.end += grown;
            }
        }
        left.push_back(std::move(moved));
    }
    findings_ = std::move(left);
    at_ = 0;
    show_current();
}

void SpellingDialog::add_to_dictionary() {
    const service::spell::Finding* f = current();
    if (f == nullptr || f->kind != "spelling") return;
    const auto store = settings();
    QStringList words = store->value(QStringLiteral("type/user_dictionary")).toString().split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    const QString w = qs(f->word);
    if (!words.contains(w, Qt::CaseInsensitive)) {
        words << w;
        std::sort(words.begin(), words.end(), [](const QString& a, const QString& b) { return a.toLower() < b.toLower(); });
        store->setValue(QStringLiteral("type/user_dictionary"), words.join(QLatin1Char('\t')));
    }
    const std::string low = lowered(f->word);
    findings_.erase(std::remove_if(findings_.begin() + static_cast<std::ptrdiff_t>(at_), findings_.end(),
                                   [&](const service::spell::Finding& g) { return g.kind == "spelling" && lowered(g.word) == low; }),
                    findings_.end());
    show_current();
}

// --- 表記の表 --------------------------------------------------------------------------------------------------------------

NotationDialog::NotationDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("notation_dialog"));
    setWindowTitle(QStringLiteral("表記の表"));
    resize(420, 360);
    table = new QTableWidget(0, 2);
    table->setHorizontalHeaderLabels({QStringLiteral("この書き方を"), QStringLiteral("こう直す")});
    table->horizontalHeader()->setStretchLastSection(true);
    for (const service::spell::Notation& n : notation_table()) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, new QTableWidgetItem(qs(n.from)));
        table->setItem(row, 1, new QTableWidgetItem(qs(n.to)));
    }
    auto* add = new QPushButton(QStringLiteral("行を足す"));
    connect(add, &QPushButton::clicked, this, [this] {
        table->insertRow(table->rowCount());
        table->setCurrentCell(table->rowCount() - 1, 0);
    });
    auto* remove = new QPushButton(QStringLiteral("行を消す"));
    connect(remove, &QPushButton::clicked, this, [this] {
        if (table->currentRow() >= 0) table->removeRow(table->currentRow());
    });
    auto* note = new QLabel(QStringLiteral("台詞とテキストレイヤーで、左の書き方を右に直すよう知らせます（例: 出来る → できる、ヶ月 → か月）。このパソコンの設定に覚えます。"));
    note->setWordWrap(true);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* rows = new QHBoxLayout;
    rows->addWidget(add);
    rows->addWidget(remove);
    rows->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(table, 1);
    layout->addLayout(rows);
    layout->addWidget(box);
}

void NotationDialog::save() {
    Json raw = Json::array();
    for (int row = 0; row < table->rowCount(); ++row) {
        const QTableWidgetItem* a = table->item(row, 0);
        const QTableWidgetItem* b = table->item(row, 1);
        if (a == nullptr || a->text().isEmpty()) continue;
        raw.push_back(Json{{"from", a->text().toStdString()}, {"to", b != nullptr ? b->text().toStdString() : std::string()}});
    }
    const auto table_now = service::spell::notations_of(raw);
    settings()->setValue(QStringLiteral("type/notations"), QString::fromStdString(service::spell::notations_json(table_now).dump()));
}

// --- 見つからないフォントを解決 -------------------------------------------------------------------------------------------

MissingFontsDialog::MissingFontsDialog(MainWindow* window, const Json& missing) : QDialog(window), missing_(missing) {
    setObjectName(QStringLiteral("missing_fonts_dialog"));
    setWindowTitle(QStringLiteral("見つからないフォントを解決"));
    resize(560, 320);
    table = new QTableWidget(0, 3);
    table->setHorizontalHeaderLabels({QStringLiteral("見つからないフォント"), QStringLiteral("使っている文字"), QStringLiteral("置き換えるフォント")});
    table->horizontalHeader()->setStretchLastSection(true);
    // (the computer's faces: one of the same PostScript name, else of the same family, chosen first)
    Json families = Json::array();
    try {
        families = service::briefs::type_fonts();
    } catch (const std::exception&) {
    }
    for (const Json& entry : missing_) {
        const int row = table->rowCount();
        table->insertRow(row);
        const QString font = qs(entry.value("font", std::string()));
        table->setItem(row, 0, new QTableWidgetItem(QFileInfo(font).fileName().isEmpty() ? font : QFileInfo(font).fileName()));
        table->item(row, 0)->setToolTip(font);
        table->setItem(row, 1, new QTableWidgetItem(QString::number(entry.value("texts", Json(0)).get<std::int64_t>())));
        auto* box = new QComboBox;
        box->addItem(QStringLiteral("いつもの書体（置き換えない字も既定で描きます）"), QString());
        for (const auto& [key, label] : lettering::bundled_fonts()) box->addItem(label, QStringLiteral("key:") + key);
        int pick = 0;
        const std::string postscript = entry.value("postscript", Json()).is_string() ? entry["postscript"].get<std::string>() : std::string();
        const std::string family = entry.value("family", Json()).is_string() ? entry["family"].get<std::string>() : std::string();
        for (const Json& fam : families) {
            for (const Json& face : fam.value("styles", Json::array())) {
                const QString path = qs(face.value("path", std::string()));
                const int index = face.value("index", Json(0)).is_number_integer() ? face.value("index", Json(0)).get<int>() : 0;
                const QString style = face.value("style", Json()).is_string() ? qs(face["style"].get<std::string>()) : QString();
                box->addItem(QStringLiteral("%1 %2").arg(qs(fam.value("family", std::string())), style),
                             QStringLiteral("face:") + path + QChar(0x1f) + QString::number(index));
                const bool same_ps = !postscript.empty() && face.value("postscript", Json()) == Json(postscript);
                const bool same_family = !family.empty() && fam.value("family", Json()) == Json(family);
                if (same_ps || (pick == 0 && same_family)) pick = box->count() - 1;
            }
        }
        box->setCurrentIndex(pick);
        table->setCellWidget(row, 2, box);
        choices.push_back(box);
    }
    auto* note = new QLabel(QStringLiteral("原稿の文字が使っている書体のうち、このパソコンに無いものです。置き換えると、その書体の文字は選んだ書体で描かれます（元に戻すで戻せます）。"));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note);
    layout->addWidget(table, 1);
    layout->addWidget(buttons);
}

Json MissingFontsDialog::map() const {
    Json out = Json::object();
    for (std::size_t i = 0; i < choices.size() && i < missing_.size(); ++i) {
        const std::string from = missing_[i].value("font", std::string());
        const QString data = choices[i]->currentData().toString();
        if (data.isEmpty()) {
            out[from] = nullptr;
        } else if (data.startsWith(QLatin1String("key:"))) {
            out[from] = data.mid(4).toStdString();
        } else {
            const QStringList parts = data.mid(5).split(QChar(0x1f));
            Json to{{"font", parts.value(0).toStdString()}};
            if (parts.value(1).toInt() > 0) to["face_index"] = parts.value(1).toInt();
            out[from] = to;
        }
    }
    return out;
}

}  // namespace genko::app
