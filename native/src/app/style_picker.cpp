#include "app/style_picker.hpp"

#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <exception>

#include "app/ask.hpp"
#include "app/links.hpp"
#include "app/look.hpp"
#include "app/theme.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "service/stylecat.hpp"

namespace genko::app {

using core::Json;

namespace {

const QSize kThumb(72, 96);
const QSize kPicture(240, 320);

QString qs(const std::string& text) { return QString::fromStdString(text); }

// A thumbnail that keeps its own colours when its row is chosen (not tinted with the selection).
QIcon still_icon(const QPixmap& pixmap) {
    QIcon icon(pixmap);
    icon.addPixmap(pixmap, QIcon::Selected);
    return icon;
}

// node.get(key) for a branch (a dict: tree() lists only those).
Json field(const Json& node, const char* key) { return node.is_object() ? node.value(key, Json()) : Json(); }

// `x or ""`, as a text.
QString text_or_blank(const Json& value) { return core::py_truthy(value) ? qs(core::py_str(value)) : QString(); }

// str(exc) of what the catalog's reading raised.
QString problem_of(const std::exception& error) { return QString::fromUtf8(error.what()); }

}  // namespace

StylePicker::StylePicker(QWidget* parent, const Json& kept, const std::string& expression)
    : QDialog(parent), kept_(core::py_truthy(kept) ? kept : Json()), expression_(expression) {
    setWindowTitle(QStringLiteral("絵柄を選ぶ"));

    where = new QLabel;
    theme::role(where, "hint");
    up = new QPushButton(QStringLiteral("上の段へ"));
    connect(up, &QPushButton::clicked, this, &StylePicker::go_up);
    auto* top = new QHBoxLayout;
    top->addWidget(up);
    top->addWidget(where, 1);
    find = new QLineEdit;
    find->setPlaceholderText(QStringLiteral("この段から探す（名前・説明）。サイトのページの URL を貼ると、その絵柄へ"));
    find->setClearButtonEnabled(true);
    connect(find, &QLineEdit::textChanged, this, &StylePicker::filter);
    list = new QListWidget;
    list->setIconSize(kThumb);
    list->setSpacing(2);
    connect(list, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item, QListWidgetItem*) {
        show_card(item != nullptr ? std::optional<std::string>(item->data(Qt::UserRole).toString().toStdString()) : std::nullopt);
    });
    connect(list, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { go_down(item->data(Qt::UserRole).toString().toStdString()); });
    note = new QLabel(QStringLiteral("ダブルクリックで下の段へ。どの段でも「この絵柄にする」で決められます（下の段は描く人におまかせ）。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    auto* body = new QVBoxLayout;
    body->addLayout(top);
    body->addWidget(find);
    body->addWidget(list, 1);
    body->addWidget(note);

    picture = new QLabel;
    picture->setAlignment(Qt::AlignCenter);
    picture->setFixedHeight(kPicture.height());
    title = new QLabel;
    theme::role(title, "heading");
    title->setWordWrap(true);
    summary = new QLabel;
    summary->setWordWrap(true);
    fit = new QLabel;
    fit->setWordWrap(true);
    words = new QLabel;
    words->setWordWrap(true);
    words->setTextInteractionFlags(Qt::TextSelectableByMouse);
    theme::role(words, "hint");
    show_words = new QPushButton(QStringLiteral("依頼の言葉を見る"));
    show_words->setCheckable(true);
    theme::role_prop(show_words, "quiet", true);
    show_words->setToolTip(QStringLiteral("この絵柄が絵の依頼に書く言葉（長い）を見ます"));
    site = new QPushButton(QStringLiteral("サイトで見る"));
    theme::role_prop(site, "quiet", true);
    connect(site, &QPushButton::clicked, this, [this] {
        if (const auto id = chosen_id()) ask::open_url(QUrl(qs(service::stylecat::page_url(*id))));
    });
    words_area = new QScrollArea;  // (the words are long: they scroll, the picture stays)
    words_area->setWidget(words);
    words_area->setWidgetResizable(true);
    words_area->setFrameShape(QFrame::NoFrame);
    auto* side = new QVBoxLayout;
    side->addWidget(picture);
    side->addWidget(title);
    side->addWidget(summary);
    side->addWidget(fit);
    side->addWidget(words_area, 1);
    words_area->hide();  // (the words for the image tool are long: shown when asked)
    connect(show_words, &QPushButton::toggled, words_area, &QWidget::setVisible);
    auto* links_row = new QHBoxLayout;
    links_row->addWidget(show_words);
    links_row->addWidget(site);
    links_row->addStretch(1);
    side->addLayout(links_row);
    side->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok = buttons->button(QDialogButtonBox::Ok);
    ok->setText(QStringLiteral("この絵柄にする"));
    ok->setEnabled(false);
    cancel = buttons->button(QDialogButtonBox::Cancel);
    cancel->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        chosen = chosen_id();
        if (chosen) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* extra = new QWidget;
    // (Python's footer has the two in a row beside the buttons; here one over the other, so the dialog fits a screen of
    // 512 points across — 1024 at 200 %: UX-02)
    auto* row = new QVBoxLayout(extra);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(2);
    plain = new QPushButton(QStringLiteral("Genko の言葉に戻す"));
    theme::role_prop(plain, "quiet", true);
    plain->setVisible(!kept_.is_null());
    connect(plain, &QPushButton::clicked, this, [this] {
        back_to_genko = true;
        accept();
    });
    links_button = new QPushButton(QStringLiteral("サイトのボタンで開けるようにする"));
    theme::role_prop(links_button, "quiet", true);
    links_button->setToolTip(QStringLiteral("サイトの「この絵柄を使う」を押すと、この Genko に届くようにします（このパソコンに 1 回だけ）"));
    connect(links_button, &QPushButton::clicked, this, [this] {
        const auto [done, said] = links::register_links();
        message(said, !done);
    });
    row->addWidget(plain, 0, Qt::AlignLeft);
    row->addWidget(links_button, 0, Qt::AlignLeft);

    QString now = QStringLiteral("今の絵柄: Genko の言葉（カタログから選んでいない）");
    if (!kept_.is_null()) {
        QStringList path;
        const Json parts = field(kept_, "path");
        if (core::py_truthy(parts)) {
            for (const Json& part : core::iterate(parts)) path << qs(core::py_str(part));
        } else {
            path << qs(core::py_str(kept_.is_object() && kept_.contains("title") ? kept_["title"] : Json("")));
        }
        now = QStringLiteral("今の絵柄: %1（版 %2）").arg(path.join(QStringLiteral("・")), qs(core::py_str(field(kept_, "version"))));
    }
    look::frame(this,
                look::header(QStringLiteral("絵柄を選ぶ"), QStringLiteral("マンガの絵柄カタログから選びます。選んだ絵柄の言葉と見本は、この原稿に写して"
                                                                   "残します（サイトが変わっても、この原稿の絵柄は変わりません）。") + now),
                body, look::card(side, QStringLiteral("選んでいる絵柄")), look::footer(buttons, extra));
    look::fit_to_screen(this, QSize(820, 600));
    message(QStringLiteral("絵柄カタログを読んでいます…"));
    run([] {
        Result out;
        out.json = service::stylecat::tree();
        return out;
    }, [this](const Result& result) { got_tree(result); });
}

StylePicker::~StylePicker() = default;

// --- reading in the background ---------------------------------------------------------------------------------------

void StylePicker::run(std::function<Result()> job, std::function<void(const Result&)> then) {
    ++reading_;
    auto* watcher = new QFutureWatcher<Result>(this);  // (gone with the dialog: a late answer is dropped)
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, then = std::move(then)] {
        --reading_;
        const Result result = watcher->result();
        watcher->deleteLater();
        then(result);
    });
    watcher->setFuture(QtConcurrent::run([job = std::move(job)]() -> Result {
        try {
            return job();
        } catch (const std::exception& error) {
            Result failed;
            failed.problem = problem_of(error);
            if (failed.problem.isEmpty()) failed.problem = QStringLiteral("絵柄カタログを読めません");
            return failed;
        }
    }));
}

void StylePicker::message(const QString& text, bool error) {
    where->setText(text);
    theme::role(where, error ? "error" : "hint");
}

void StylePicker::got_tree(const Result& result) {
    if (!result.problem.isEmpty()) {
        message(result.problem, true);
        return;
    }
    order_.clear();
    nodes_.clear();
    for (const Json& node : core::iterate(result.json)) {
        const std::string id = core::py_str(field(node, "id"));
        if (!nodes_.contains(id)) order_.push_back(id);
        nodes_[id] = node;  // (a later branch of the same id wins, in the first one's place: Python's dict)
    }
    std::optional<std::string> start;
    std::optional<std::string> select;
    if (!kept_.is_null()) {
        const Json kept_id = field(kept_, "id");
        if (!kept_id.is_null()) select = core::py_str(kept_id);
        if (select && nodes_.contains(*select)) start = parent_of(*select);  // (open where the book's style is)
    }
    show_list(start, select);
}

// --- the list -------------------------------------------------------------------------------------------------------

std::optional<std::string> StylePicker::parent_of(const std::string& node_id) const {
    const auto it = nodes_.find(node_id);
    if (it == nodes_.end()) return std::nullopt;
    const Json parent = field(it->second, "parent");
    if (parent.is_null()) return std::nullopt;
    return core::py_str(parent);
}

std::vector<std::string> StylePicker::children(const std::optional<std::string>& parent) const {
    std::vector<std::string> out;
    for (const std::string& id : order_) {
        if (parent_of(id) == parent) out.push_back(id);
    }
    return out;
}

void StylePicker::show_list(const std::optional<std::string>& parent, const std::optional<std::string>& select) {
    here_ = parent;
    list->clear();
    QStringList trail;
    std::optional<std::string> at = parent;
    while (at && nodes_.contains(*at)) {
        const Json& node = nodes_.at(*at);
        trail.prepend(qs(core::py_str(node.contains("title") ? node["title"] : Json(*at))));
        at = parent_of(*at);
    }
    QString level;
    if (!parent) {
        level = QStringLiteral("1段目（ジャンル）");
    } else if (nodes_.contains(*parent)) {
        const Json depth = field(nodes_.at(*parent), "level");
        if (core::py_equals(depth, Json(1))) level = QStringLiteral("2段目");
        if (core::py_equals(depth, Json(2))) level = QStringLiteral("3段目（仕上げの絵柄）");
    }
    trail.prepend(QStringLiteral("絵柄カタログ"));
    message(trail.join(QStringLiteral(" › ")) + QStringLiteral(" — ") + level);
    up->setEnabled(parent.has_value());
    for (const std::string& id : children(parent)) {
        const Json& child = nodes_.at(id);
        const auto count = children(id).size();
        auto* item = new QListWidgetItem(qs(core::py_str(field(child, "title"))) + QLatin1Char('\n') + text_or_blank(field(child, "summary")) +
                                         (count > 0 ? QStringLiteral("（下に %1 種）").arg(count) : QString()));
        item->setData(Qt::UserRole, qs(id));
        item->setSizeHint(QSize(0, kThumb.height() + 8));
        list->addItem(item);
        thumb(id);
        if (select && id == *select) list->setCurrentItem(item);
    }
    filter(find->text());
}

void StylePicker::filter(const QString& typed) {
    // (a pasted page URL or genko:// link goes to that style)
    if (typed.contains(QStringLiteral("://"))) {
        std::optional<std::string> found;
        try {
            found = service::stylecat::style_from_link(Json(typed.toStdString()));
        } catch (const service::stylecat::CatalogError&) {
            found.reset();
        }
        if (found && nodes_.contains(*found)) {
            find->blockSignals(true);
            find->clear();
            find->blockSignals(false);
            show_list(parent_of(*found), found);
            return;
        }
    }
    // (only the rows whose name or summary has all the words: the third level has twenty-odd styles)
    static const QRegularExpression spaces(QStringLiteral("\\s+"), QRegularExpression::UseUnicodePropertiesOption);
    const QStringList wanted = typed.split(spaces, Qt::SkipEmptyParts);
    for (int row = 0; row < list->count(); ++row) {
        QListWidgetItem* item = list->item(row);
        const bool all = std::all_of(wanted.begin(), wanted.end(), [&](const QString& w) { return item->text().contains(w); });
        item->setHidden(!wanted.isEmpty() && !all);
    }
}

void StylePicker::thumb(const std::string& node_id) {
    const Json url = field(nodes_.at(node_id), "thumbnail_url");
    if (!core::py_truthy(url)) return;
    const std::string key = "t:" + node_id;
    const auto set_icon = [this, node_id](const QPixmap& pixmap) {
        for (int row = 0; row < list->count(); ++row) {  // (the row, if the list still shows it)
            if (list->item(row)->data(Qt::UserRole).toString().toStdString() == node_id) list->item(row)->setIcon(still_icon(pixmap));
        }
    };
    if (const auto it = pictures_.find(key); it != pictures_.end()) {
        set_icon(it->second);
        return;
    }
    const std::string address = core::py_str(url);
    run([address] {
        Result out;
        out.bytes = service::stylecat::get(address);
        return out;
    }, [this, key, set_icon](const Result& result) {
        if (!result.problem.isEmpty() || result.bytes.empty()) return;
        QPixmap pixmap;
        if (!pixmap.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()), static_cast<uint>(result.bytes.size()))) return;
        pictures_[key] = pixmap;
        set_icon(pixmap);
    });
}

void StylePicker::go_down(const std::string& node_id) {
    if (!node_id.empty() && !children(node_id).empty()) show_list(node_id);
}

void StylePicker::go_up() {
    if (!here_) return;
    const std::string was = *here_;
    show_list(parent_of(was), was);
}

// --- the card -------------------------------------------------------------------------------------------------------

std::optional<std::string> StylePicker::chosen_id() const {
    const QListWidgetItem* item = list->currentItem();
    if (item == nullptr) return std::nullopt;
    return item->data(Qt::UserRole).toString().toStdString();
}

void StylePicker::show_card(const std::optional<std::string>& node_id) {
    ok->setEnabled(node_id.has_value());
    if (!node_id) return;
    const auto found = nodes_.find(*node_id);
    const Json node = found == nodes_.end() ? Json::object() : found->second;
    title->setText(qs(core::py_str(node.contains("title") ? node["title"] : Json(*node_id))));
    summary->setText(text_or_blank(field(node, "summary")));
    fit->setText(QString());
    words->setText(QString());
    picture->setPixmap(QPixmap());
    if (const auto it = details_.find(*node_id); it != details_.end()) {
        detail(*node_id, it->second);
        return;
    }
    words->setText(QStringLiteral("読んでいます…"));
    const std::string id = *node_id;
    run([id] {
        Result out;
        out.json = service::stylecat::style(Json(id));
        return out;
    }, [this, id](const Result& result) {
        if (!result.problem.isEmpty()) {
            if (chosen_id() == id) words->setText(result.problem);
            return;
        }
        details_[id] = result.json;
        if (chosen_id() == id) detail(id, result.json);
    });
}

void StylePicker::detail(const std::string& node_id, const Json& data) {
    const Json prompt = field(data, "prompt");
    words->setText(text_or_blank(prompt.is_object() ? prompt.value("ja", Json()) : Json()));
    const Json expression = data.is_object() && data.contains("expression") ? data["expression"] : Json("mono");
    const QString colour = core::py_equals(field(data, "expression"), Json("mono")) ? QStringLiteral("白黒") : QStringLiteral("カラー");
    const bool clash = core::py_equals(expression, Json("mono")) && expression_ == "color";
    fit->setText(colour + QStringLiteral("・版 ") + qs(core::py_str(field(data, "version"))) +
                 (clash ? QStringLiteral("　この原稿はカラーなので、絵の依頼には使われません") : QString()));
    theme::role(fit, clash ? "error" : "hint");
    const std::string key = "s:" + node_id;
    if (const auto it = pictures_.find(key); it != pictures_.end()) {
        show_picture(it->second);
        return;
    }
    const Json samples = core::py_truthy(field(data, "samples")) ? field(data, "samples") : Json::array();
    const Json reference = field(data, "reference");
    // (isinstance(reference, int): a bool is one too)
    const std::int64_t index = reference.is_number_integer() ? reference.get<std::int64_t>() : reference.is_boolean() ? (reference.get<bool>() ? 1 : 0) : 0;
    if (!samples.is_array() || index < 0 || index >= static_cast<std::int64_t>(samples.size())) return;
    const Json& sample = samples[static_cast<std::size_t>(index)];
    const Json url = sample.is_object() ? sample.value("url", Json()) : Json();
    if (!core::py_truthy(url)) return;
    const std::string address = core::py_str(url);
    run([address] {
        Result out;
        out.bytes = service::stylecat::get(address);
        return out;
    }, [this, key, node_id](const Result& result) {
        if (!result.problem.isEmpty() || result.bytes.empty()) return;
        QPixmap pixmap;
        if (!pixmap.loadFromData(reinterpret_cast<const uchar*>(result.bytes.data()), static_cast<uint>(result.bytes.size()))) return;
        pictures_[key] = pixmap;
        if (chosen_id() == node_id) show_picture(pixmap);
    });
}

void StylePicker::show_picture(const QPixmap& pixmap) {
    picture->setPixmap(pixmap.scaled(kPicture, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

}  // namespace genko::app
