#include "app/studio_widgets.hpp"

#include <QScopeGuard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <map>

#include "app/ask.hpp"
#include "app/canvas.hpp"
#include "app/export_dialog.hpp"
#include "app/icons.hpp"
#include "app/main_window.hpp"
#include "app/reply_box.hpp"
#include "app/save_status.hpp"
#include "app/theme.hpp"
#include "app/viewer.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "render/page.hpp"
#include "service/atari.hpp"
#include "service/genreq.hpp"
#include "service/preview.hpp"
#include "service/wording.hpp"
#include "storage/asset_store.hpp"

namespace genko::app {

using core::Json;
using review_model::InboxItem;
namespace fs = std::filesystem;

namespace {

QString qs(const std::string& text) { return QString::fromStdString(text); }
QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }

Json at(const Json& object, std::string_view key) {
    if (!object.is_object()) return Json();
    const auto it = object.find(std::string(key));
    return it == object.end() ? Json() : *it;
}

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

// theme.still_icon: a picture that stays itself when its row is chosen (Qt tints chosen icons with the accent otherwise).
QIcon still_icon(const QPixmap& pixmap) {
    QIcon icon;
    for (const QIcon::Mode mode : {QIcon::Normal, QIcon::Selected, QIcon::Active}) icon.addPixmap(pixmap, mode);
    return icon;
}

// theme.iconic: a button with its picture.
QPushButton* iconic(QPushButton* button, const char* name) {
    button->setIcon(icons::icon(name));
    return button;
}

// HOW: how to answer each kind of request (on hover over its words).
QString how(const QString& kind) {
    static const std::map<QString, QString> table = {
        {QStringLiteral("name"), QStringLiteral("コマ割り・読み順・台詞の位置を見ます。大きく見る で 1 ページずつ確かめられます。")},
        {QStringLiteral("art"), QStringLiteral("印刷と同じ見た目（網点は平らなグレー）で、絵がネームどおりか、人物が設定画に似ているかを見ます。")},
        {QStringLiteral("sheet"), QStringLiteral("候補から 1 枚選んで承認します。選んだ絵の顔のアップが、以後の作画の参照になります。")},
        {QStringLiteral("export"), QStringLiteral("書き出す前の点検結果です。止める理由が無ければ、書き出す… で正式に書き出します。")},
        {QStringLiteral("help"), QStringLiteral("AI からの相談です。返事を書いて「返事を送る」と、AI の作業に指示として届きます。")},
        {QStringLiteral("proposal"), QStringLiteral("アタリから読み取った提案です。確定するまで原稿は変わりません。")},
    };
    const auto it = table.find(kind);
    return it == table.end() ? QString() : it->second;
}

const core::Page* page_by_index(const core::Document& doc, std::int64_t index) {
    for (const auto& page : doc.pages) {
        if (page->index == core::Num(index)) return page.get();
    }
    return nullptr;
}

std::int64_t page_number(const Json& value) {
    try {
        return core::py_int(value);
    } catch (const core::Error&) {
        return 0;
    }
}

// What a person's words to the AI look like before they go (I13): who gets them, and the words.
bool confirm_send(QWidget* parent, const QString& title, const QString& to, const QString& text) {
    return ask::question(parent, title,
                         QStringLiteral("%1 に、次の言葉を送ります。\n\n%2\n\n送りますか？").arg(to.isEmpty() ? QStringLiteral("AI") : to, text.trimmed()));
}

}  // namespace

QString draft_book_key(const MainWindow& window) {
    const Session& session = window.session();
    const QString key = session.path() ? qpath(*session.path()) : QStringLiteral("book:") + qs(window.book().book_id);
    Drafts::instance().place(key, session.path(), session.read_only_reason().empty());
    return key;
}

std::optional<QImage> asset_image(const std::optional<fs::path>& project, const std::string& ref) {
    if (!project || ref.empty()) return std::nullopt;
    try {
        const auto data = storage::AssetStore(*project).get_bytes(ref, ".png");
        if (!data) return std::nullopt;
        QImage image = QImage::fromData(reinterpret_cast<const uchar*>(data->data()), static_cast<int>(data->size()));
        if (image.isNull()) return std::nullopt;
        return image;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

QImage thumbnail(const QImage& image, QSize box) {
    if (image.width() <= box.width() && image.height() <= box.height()) return image;
    return image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QStringList brief_words(const Json& panel) {
    QStringList out;
    Json v;
    try {
        v = service::genreq::vocab();
    } catch (const std::exception&) {
        v = Json::object();
    }
    for (const char* key : {"shot", "angle"}) {
        const Json code = at(panel, key);
        if (!core::py_truthy(code)) continue;
        const Json table = at(v, key);
        const Json entry = code.is_string() ? at(table, code.get<std::string>()) : Json();
        const Json ja = at(entry, "ja");
        out << qs(core::py_str(core::py_truthy(ja) ? ja : code));
    }
    for (const char* key : {"action", "emotion"}) {
        const Json value = at(panel, key);
        if (core::py_truthy(value)) out << qs(core::py_str(value));
    }
    return out;
}

namespace {

// comfort.toast's note: a click closes it.
class ToastLabel : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void mousePressEvent(QMouseEvent*) override { deleteLater(); }
};

}  // namespace

QLabel* toast(MainWindow* window, const QString& text, int ms) {
    const theme::Tokens t = theme::tokens();
    QWidget* canvas = window->canvas();
    auto* note = new ToastLabel(text, canvas);
    note->setObjectName(QStringLiteral("toast"));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("background:%1; color:%2; border:1px solid %3; border-radius:8px; padding:8px 12px;").arg(t.panel, t.text, t.accent));
    note->setMaximumWidth(460);
    note->adjustSize();
    note->setFixedWidth(std::min(460, note->fontMetrics().horizontalAdvance(text.split(QLatin1Char('\n')).front()) + 34));
    note->adjustSize();
    note->move(std::max(8, (canvas->width() - note->width()) / 2), std::max(8, canvas->height() - note->height() - 24));
    note->show();
    QTimer::singleShot(ms, note, &QObject::deleteLater);
    return note;
}

// --- process bar -----------------------------------------------------------------------------------------------------

namespace {

// A count in the status bar that opens what it counts.
class Chip : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void()> clicked;

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && clicked) clicked();
        QLabel::mouseReleaseEvent(event);
    }
};

}  // namespace

ProcessBar::ProcessBar() {
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(6, 2, 6, 2);
    layout_->setSpacing(6);
}

void ProcessBar::refresh(const core::Document& doc) {
    for (QLabel* label : labels_) {
        layout_->removeWidget(label);
        label->setParent(nullptr);  // gone now, not at the next turn of the event loop
        label->deleteLater();
    }
    labels_.clear();
    const auto pending = review_model::inbox(doc).size();
    struct ChipText {
        QString text;
        const char* kind;
        QString tip;
    };
    std::vector<ChipText> chips = {{QStringLiteral("承認箱 <b>%1</b>").arg(pending), pending ? "badge-warn" : "badge",
                                    QStringLiteral("AI からの承認依頼と相談。押すと右上に承認箱が出て、1 件ずつ見て決められます")}};
    for (const auto& [text, count] : review_model::progress(doc)) {
        if (count == 0) continue;
        const QString words = qs(text);
        const bool warn = words.contains(QStringLiteral("待ち")) || words.contains(QStringLiteral("未承認"));
        chips.push_back({QStringLiteral("%1 <b>%2</b>").arg(words).arg(count), warn ? "badge-warn" : "badge", QString()});
    }
    for (std::size_t n = 0; n < chips.size(); ++n) {
        QLabel* label = nullptr;
        if (n == 0) {
            auto* chip = new Chip(chips[n].text);
            chip->clicked = [this] { emit openBox(); };
            chip->setCursor(Qt::PointingHandCursor);
            label = chip;
        } else {
            label = new QLabel(chips[n].text);
        }
        theme::role(label, chips[n].kind);
        if (!chips[n].tip.isEmpty()) label->setToolTip(chips[n].tip);
        layout_->addWidget(label);
        labels_.push_back(label);
    }
}

// --- approval box ----------------------------------------------------------------------------------------------------

PreviewLabel::PreviewLabel() {
    setAlignment(Qt::AlignCenter);
    setMinimumHeight(120);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    setToolTip(QStringLiteral("ダブルクリックで大きく見る"));
}

void PreviewLabel::set_image(const std::optional<QPixmap>& pix) {
    source_ = pix && !pix->isNull() ? pix : std::nullopt;
    fit();
}

void PreviewLabel::fit() {
    if (!source_) {
        if (!pixmap().isNull()) setPixmap(QPixmap());  // (clearing a picture; words put here instead stay)
        return;
    }
    setPixmap(source_->scaled(std::max(40, width() - 4), std::max(40, height() - 4), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void PreviewLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    fit();
    if (corner != nullptr) {
        corner->adjustSize();
        corner->move(width() - corner->width() - 8, 8);
    }
}

void PreviewLabel::mouseDoubleClickEvent(QMouseEvent*) { emit activated(); }

ApprovalBox::ApprovalBox(MainWindow* window) : window_(window) {
    list = new QListWidget;
    connect(list, &QListWidget::currentRowChanged, this, [this](int row) { show_item(row); });
    detail = new QLabel;
    detail->setWordWrap(true);
    detail->setTextFormat(Qt::RichText);
    page_pick = new QComboBox;
    connect(page_pick, &QComboBox::currentIndexChanged, this, [this](int) { page_changed(); });
    preview = new PreviewLabel;
    preview->setObjectName(QStringLiteral("approvalPreview"));
    connect(preview, &PreviewLabel::activated, this, &ApprovalBox::open_viewer);
    preview->setMinimumHeight(170);
    // the page as wide as the box, with 大きく見る over its corner (not a row of its own)
    big = iconic(new QPushButton(QStringLiteral("大きく見る"), preview), "expand");
    big->setObjectName(QStringLiteral("overPicture"));
    big->setCursor(Qt::PointingHandCursor);
    connect(big, &QPushButton::clicked, this, &ApprovalBox::open_viewer);
    preview->corner = big;
    choices = new QListWidget;
    choices->setViewMode(QListWidget::IconMode);
    choices->setIconSize(QSize(120, 150));
    choices->setResizeMode(QListWidget::Adjust);
    choices->setMaximumHeight(200);
    connect(choices, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { open_viewer(); });
    reason = new ReplyEdit;
    reason->setObjectName(QStringLiteral("approvalReply"));
    connect(reason, &ReplyEdit::sendRequested, this, [this] {
        if (back_button->isVisible() && back_button->isEnabled()) send_back();
    });
    approve_button = new QPushButton(QStringLiteral("承認"));  // (the accent button: its words alone)
    connect(approve_button, &QPushButton::clicked, this, &ApprovalBox::approve);
    back_button = iconic(new QPushButton(QStringLiteral("差し戻し")), "back");
    connect(back_button, &QPushButton::clicked, this, &ApprovalBox::send_back);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(back_button);
    buttons->addStretch(1);
    buttons->addWidget(approve_button);
    auto* head = new QHBoxLayout;
    head->addWidget(page_pick, 1);
    empty = new QLabel(QStringLiteral("承認を待っている依頼はありません。\nAI が依頼を出すと、ここに届きます。"));
    empty->setAlignment(Qt::AlignCenter);
    theme::role(empty, "hint");
    // the box's name and how many wait, over the list
    auto* title = new QLabel(QStringLiteral("承認待ち"));
    theme::role(title, "heading");
    title->setToolTip(QStringLiteral("AI からの承認の依頼と相談。1 件ずつ見て決めます"));
    count = new QLabel;
    theme::role(count, "badge-warn");
    auto* top = new QHBoxLayout;
    top->setSpacing(8);
    top->addWidget(title);
    top->addWidget(count);
    top->addStretch(1);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);
    layout->addLayout(top);
    layout->addWidget(list);
    layout->addWidget(empty);
    layout->addWidget(detail);
    // the answer (reason and buttons) over the pictures: many candidates on a short screen never push the approve button
    // out of reach
    layout->addWidget(reason);
    layout->addLayout(buttons);
    layout->addLayout(head);
    layout->addWidget(preview, 3);
    layout->addWidget(choices);
    theme::primary(approve_button);
}

QString ApprovalBox::book_key() const { return draft_book_key(*window_); }

void ApprovalBox::refresh() {
    const auto previous = current();
    items_ = review_model::inbox(window_->book());
    list->blockSignals(true);
    list->clear();
    for (const InboxItem& item : items_) list->addItem(qs(item.title()));
    list->blockSignals(false);
    count->setText(QString::number(items_.size()));
    count->setVisible(!items_.empty());
    // the list as tall as its rows (up to four), not a fixed block of empty space under one request
    const int rows = std::max(1, std::min(4, static_cast<int>(items_.size())));
    list->setFixedHeight(rows * std::max(list->sizeHintForRow(0), 24) + 6);
    int row = items_.empty() ? -1 : 0;
    if (previous) {
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].ticket_id == previous->ticket_id) row = static_cast<int>(i);
        }
    }
    list->blockSignals(true);
    list->setCurrentRow(row);
    list->blockSignals(false);
    show_item(row, false);
}

std::optional<InboxItem> ApprovalBox::current() const {
    const int row = list->currentRow();
    if (row < 0 || row >= static_cast<int>(items_.size())) return std::nullopt;
    return items_[static_cast<std::size_t>(row)];
}

QString ApprovalBox::kind_of(const InboxItem& item) const {
    return item.kind == "help" || item.kind == "proposal" ? qs(item.kind) : qs(item.gate_text());
}

void ApprovalBox::show_item(int, bool follow) {
    const auto item = current();
    choices->clear();
    page_pick->blockSignals(true);
    // (C++: the page picked stays picked when the same request is shown again — Python's box goes back to its first page
    // when the window follows the pick)
    const QVariant picked = page_pick->currentData();
    const QString picked_for = page_pick->property("ticket").toString();
    page_pick->clear();
    if (item) {
        for (const Json& page : item->pages) page_pick->addItem(QStringLiteral("%1 ページ").arg(qs(core::py_str(page))), QVariant::fromValue<qlonglong>(page_number(page)));
        page_pick->setProperty("ticket", qs(item->ticket_id));
        if (!follow && picked.isValid() && picked_for == qs(item->ticket_id)) {
            const int again = page_pick->findData(picked);
            if (again >= 0) page_pick->setCurrentIndex(again);
        }
    }
    page_pick->setVisible(item && item->pages.size() > 1);
    page_pick->blockSignals(false);
    const bool has = item.has_value();
    for (QWidget* widget : std::initializer_list<QWidget*>{list, detail, preview, reason, approve_button, back_button, big}) widget->setVisible(has);
    empty->setVisible(!has);
    if (!item) {
        reason->bind(book_key(), QString());
        choices->setVisible(false);
        preview->set_image(std::nullopt);
        return;
    }
    reason->bind(book_key(), qs(item->ticket_id));  // (the words typed for this request come back)
    const QString kind = kind_of(*item);
    const bool exporting = item->gate_text() == "export";
    approve_button->setEnabled(true);
    back_button->setEnabled(!exporting);
    back_button->setVisible(!exporting);
    approve_button->setText(kind == QLatin1String("export")   ? QStringLiteral("書き出す…")
                            : kind == QLatin1String("help")   ? QStringLiteral("返事なしで閉じる")
                            : kind == QLatin1String("proposal") ? QStringLiteral("確定")
                                                              : QStringLiteral("承認"));
    back_button->setText(kind == QLatin1String("help") ? QStringLiteral("返事を送る") : kind == QLatin1String("proposal") ? QStringLiteral("却下") : QStringLiteral("差し戻す"));
    reason->setPlaceholderText(kind == QLatin1String("help")       ? QStringLiteral("返事（AI への指示になる）")
                               : kind == QLatin1String("proposal") ? QStringLiteral("却下の理由")
                                                                  : QStringLiteral("差し戻す理由（AI への指示になる）"));
    reason->setVisible(!exporting);
    // one clear main button: the reply for a question, the approval otherwise
    const bool answer = kind == QLatin1String("help");
    theme::role_prop(approve_button, "primary", !answer);
    theme::role_prop(back_button, "primary", answer);
    const QString muted = theme::tokens().muted;
    QString text = item->text.empty() ? QString() : qs(item->text).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")) + QStringLiteral("<br>");
    text += QStringLiteral("<small style='color:%1'>依頼: %2</small>").arg(muted, wording::actor(item->by).toHtmlEscaped());
    detail->setText(text);
    detail->setToolTip(how(kind));  // (how to answer: on hover, not in the way)
    const bool sheet = item->gate_text() == "sheet" && item->kind == "gate";
    choices->setVisible(sheet);
    big->setVisible(!exporting);
    preview->setVisible(!sheet);  // a sheet is chosen from the candidates, which get the space
    choices->setMaximumHeight(sheet ? QWIDGETSIZE_MAX : 200);
    if (sheet) sheet_choices(*item);
    if (follow && !item->pages.empty()) window_->go_to_page(static_cast<int>(page_number(item->pages.front())));
    show_preview();
    QTimer::singleShot(0, window_, [window = window_] { window->fit_box(); });  // (the box as tall as this request needs)
}

void ApprovalBox::page_changed() {
    const QVariant page = page_pick->currentData();
    if (page.isValid() && page.toLongLong() != 0) window_->go_to_page(static_cast<int>(page.toLongLong()));
    show_preview();
}

std::vector<std::pair<QString, Json>> ApprovalBox::numbered_sheets(const InboxItem& item) const {
    std::vector<std::pair<QString, Json>> out;
    for (auto& [label, cand] : review_model::numbered_sheets(window_->book(), item)) out.emplace_back(qs(label), std::move(cand));
    return out;
}

void ApprovalBox::sheet_choices(const InboxItem& item) {
    for (const auto& [label, cand] : numbered_sheets(item)) {
        auto* entry = new QListWidgetItem(label);
        const QString id = qs(core::py_str(at(cand, "id")));
        entry->setData(Qt::UserRole, id);
        entry->setToolTip(id);
        if (const auto image = asset_image(window_->session().path(), core::py_str(at(cand, "asset")))) {
            entry->setIcon(still_icon(QPixmap::fromImage(thumbnail(*image, QSize(150, 190)))));
        }
        choices->addItem(entry);
    }
    if (choices->count() > 0) choices->setCurrentRow(0);
}

const core::Page* ApprovalBox::page_of(const InboxItem& item) const {
    const QVariant chosen = page_pick->currentData();
    std::int64_t index = chosen.isValid() ? chosen.toLongLong() : 0;
    if (index == 0 && !item.pages.empty()) index = page_number(item.pages.front());
    return index == 0 ? nullptr : page_by_index(window_->book(), index);
}

QImage ApprovalBox::page_image(const InboxItem& item, const core::Page& page, int dpi) const {
    const core::Document& doc = window_->book();
    if (item.kind == "proposal") return to_qimage(service::atari::overlay_image(doc, page, dpi));
    render::RenderOptions options;
    options.mode = item.gate_text() == "name" || !page.name_ok ? "name" : "proof";
    options.skip_unported = true;  // (a preview on screen: what this build cannot draw yet is left out)
    return to_qimage(render::render_page(page, dpi, options, &doc).image);
}

void ApprovalBox::show_preview() {
    const auto item = current();
    if (!item) return;
    if (item->gate_text() == "export") {
        QString text;
        bool ok = false;
        if (const auto& path = window_->session().path()) {
            try {
                const Json report = formats::checks::studio_check(window_->book(), *path, formats::checks::StudioCheck{});
                ok = core::truthy_at(report, "ok");
                const Json errors = at(report, "errors");
                const std::size_t total = errors.is_array() ? errors.size() : 0;
                QStringList rows;
                for (std::size_t i = 0; i < total && i < 5; ++i) rows << QStringLiteral("・") + qs(core::py_str(at(errors[i], "message")));
                const QString more = total > 5 ? QStringLiteral("\nほか %1 件（全部は「点検」のタブで見られます）").arg(total - 5) : QString();
                text = ok ? QStringLiteral("点検は通っています。書き出せます。") : QStringLiteral("止めている理由:\n") + rows.join(QLatin1Char('\n')) + more;
            } catch (const std::exception& error) {
                text = QStringLiteral("点検できませんでした: %1").arg(wording::error(QString::fromUtf8(error.what())));
            }
        } else {
            text = QStringLiteral("点検できませんでした: 原稿がまだ保存されていません");
        }
        preview->set_image(std::nullopt);
        preview->setMinimumHeight(0);
        preview->setWordWrap(true);
        preview->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        preview->setText(text);
        approve_button->setEnabled(ok);
        return;
    }
    preview->setAlignment(Qt::AlignCenter);
    if (item->gate_text() == "sheet" && item->kind == "gate") {
        preview->set_image(std::nullopt);
        preview->setMinimumHeight(0);
        return;
    }
    preview->setMinimumHeight(120);
    preview->setText(QString());
    const core::Page* page = page_of(*item);
    if (page == nullptr) {
        preview->set_image(std::nullopt);
        return;
    }
    try {
        preview->set_image(QPixmap::fromImage(page_image(*item, *page, 90)));
    } catch (const std::exception& error) {
        preview->set_image(std::nullopt);
        preview->setText(QStringLiteral("この頁を描けませんでした: %1").arg(wording::error(QString::fromUtf8(error.what()))));
    }
}

void ApprovalBox::open_viewer() {
    const auto item = current();
    if (!item || item->gate_text() == "export") return;
    if (item->gate_text() == "sheet" && item->kind == "gate") {
        std::vector<ViewerItem> shown;
        for (const auto& [label, cand] : numbered_sheets(*item)) {
            if (const auto image = asset_image(window_->session().path(), core::py_str(at(cand, "asset")))) shown.emplace_back(label, image->convertToFormat(QImage::Format_RGB888));
        }
        ViewerDialog dialog(this, qs(item->title()), shown, QStringLiteral("候補を並べて比べます。選ぶのは承認箱の一覧で行います。"));
        ask::exec(&dialog);
        return;
    }
    const InboxItem chosen = *item;
    const auto load = [this, chosen](int index) -> std::vector<ViewerItem> {
        const core::Page* page = page_by_index(window_->book(), index);
        if (page == nullptr) return {};
        return {{QStringLiteral("%1 ページ").arg(index), page_image(chosen, *page, 150)}};
    };
    const core::Page* page = page_of(*item);
    if (page == nullptr) return;
    std::vector<int> pages;
    for (const Json& p : item->pages) pages.push_back(static_cast<int>(page_number(p)));
    const int index = static_cast<int>(core::py_int(page->index));
    ViewerDialog dialog(this, qs(item->title()), load(index), how(kind_of(*item)), pages, load, index);
    ask::exec(&dialog);
}

void ApprovalBox::approve() {
    const auto item = current();
    if (!item) return;
    if (item->gate_text() == "export") {
        run_export();
        return;
    }
    // (what is answered, taken before a question: the box may choose another request, or the book change, meanwhile)
    const MainWindow::Asked asked = window_->asking();
    const QString book = book_key();
    const std::string ticket = item->ticket_id;
    Json choice;
    if (QListWidgetItem* entry = choices->currentItem(); entry != nullptr) choice = entry->data(Qt::UserRole).toString().toStdString();
    if (item->kind == "help" && !reason->toPlainText().trimmed().isEmpty()) {
        // (I13: words typed and not sent are not dropped without asking)
        if (!ask::question(this, QStringLiteral("返事なしで閉じる"),
                           QStringLiteral("書きかけの返事があります（まだ送っていません）。返事を送らずに、この相談を閉じますか？"))) {
            return;
        }
    }
    const auto still = still_waiting(asked.session, ticket);
    if (!still) return;
    Json ops;
    try {
        ops = review_model::approve_ops(window_->book(), *still, window_->session().path(), choice);
    } catch (const core::PyValueError& error) {
        ask::warning(this, QStringLiteral("Genko"), QString::fromUtf8(error.what()));
        return;
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    if (window_->apply_and_commit(ops)) {
        if (still->kind == "help") forget_draft(book, ticket);
        emit changed();
    }
}

void ApprovalBox::send_back() {
    const auto item = current();
    if (!item) return;
    const MainWindow::Asked asked = window_->asking();
    const QString book = book_key();
    const std::string ticket = item->ticket_id;
    const QString text = reason->toPlainText();
    try {
        (void)review_model::send_back_ops(window_->book(), *item, text.toStdString());  // (refused at once, before asking)
    } catch (const core::PyValueError& error) {
        ask::warning(this, QStringLiteral("Genko"), QString::fromUtf8(error.what()));
        reason->setFocus();
        return;
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    if (!confirm_send(this, back_button->text(), item->by.empty() ? QString() : wording::actor(item->by), text)) return;  // (the draft stays)
    // (the ops of the book as it is now, for the request that was answered — if it is still the one chosen and waiting)
    const auto still = still_waiting(asked.session, ticket);
    if (!still) return;
    Json ops;
    try {
        ops = review_model::send_back_ops(window_->book(), *still, text.toStdString());
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    if (window_->apply_and_commit(ops)) {
        forget_draft(book, ticket);
        emit changed();
    }
}

std::optional<review_model::InboxItem> ApprovalBox::still_waiting(const std::shared_ptr<Session>& asked, const std::string& ticket) {
    const auto chosen = current();
    std::optional<review_model::InboxItem> found;
    if (window_->asking().session == asked && chosen && chosen->ticket_id == ticket) {
        for (auto& waiting : review_model::inbox(window_->book())) {
            if (waiting.ticket_id == ticket) found = std::move(waiting);
        }
    }
    if (!found) {
        window_->flash(QStringLiteral("確認の間に、承認箱の依頼か原稿が変わったため、送っていません（書いた言葉は残っています）。依頼を選び直して、もう一度行ってください。"),
                       8000, true);
    }
    return found;
}

void ApprovalBox::forget_draft(const QString& book, const std::string& ticket) {
    const QString item = qs(ticket);
    if (reason->book() == book && reason->item() == item) {
        reason->sent();
    } else {
        Drafts::instance().forget(book, item);
    }
}

void ApprovalBox::run_export() {
    // (the wait for the save below lets events through: pressed again meanwhile, it is not a second export)
    if (exporting_) return;
    exporting_ = true;
    const auto done = qScopeGuard([this] { exporting_ = false; });
    const MainWindow::Asked asked = window_->asking();
    const std::shared_ptr<Session> session = asked.session;
    // (the page in front as it was pressed, by value: what the window shows may change while the save is waited for)
    const core::Page* page = window_->current_page();
    const std::int64_t index = page != nullptr ? core::py_int(page->index) : 1;
    session->save_now();  // (Python's commit_now: what was done is written before the export reads the book)
    // (SPEC: the official export reads the book as it is saved — not while a save failed or is still going)
    if (!session->wait_saved(std::chrono::seconds(30)) || session->unsaved()) {
        window_->flash(QStringLiteral("原稿を保存できていないため、正式な書き出しをしていません: %1").arg(failure_reason(session->status())), 8000, true);
        return;
    }
    ExportDialog dialog(this, session->snapshot(), session->path(), session->actor(), true, index);
    ask::exec(&dialog);
    session->check_outside();  // (Python's session.reload(): the approval the export recorded)
    emit changed();
}

// --- panel view --------------------------------------------------------------------------------------------------------

RegionImage::RegionImage() {
    setAlignment(Qt::AlignCenter);
    setMinimumSize(200, 140);
}

QRectF RegionImage::picture_rect() const {
    const QPixmap pix = pixmap();
    if (pix.isNull()) return {};
    return QRectF((width() - pix.width()) / 2.0, (height() - pix.height()) / 2.0, pix.width(), pix.height());
}

void RegionImage::mousePressEvent(QMouseEvent* event) {
    if (drawing) start_ = end_ = event->position();
}

void RegionImage::mouseMoveEvent(QMouseEvent* event) {
    if (drawing && start_) {
        end_ = event->position();
        update();
    }
}

void RegionImage::mouseReleaseEvent(QMouseEvent* event) {
    if (!(drawing && start_)) return;
    const QRectF area = picture_rect();
    const QPointF a = *start_, b = event->position();
    start_.reset();
    end_.reset();
    if (area.isEmpty()) return;
    const double ax = (a.x() - area.x()) / area.width(), bx = (b.x() - area.x()) / area.width();
    const double ay = (a.y() - area.y()) / area.height(), by = (b.y() - area.y()) / area.height();
    const auto clamp01 = [](double v) { return std::min(1.0, std::max(0.0, v)); };
    const double x0 = clamp01(std::min(ax, bx)), x1 = clamp01(std::max(ax, bx));
    const double y0 = clamp01(std::min(ay, by)), y1 = clamp01(std::max(ay, by));
    if (x1 - x0 > 0.01 && y1 - y0 > 0.01) emit drawn(x0, y0, x1 - x0, y1 - y0);
    update();
}

void RegionImage::paintEvent(QPaintEvent* event) {
    QLabel::paintEvent(event);
    const QRectF area = picture_rect();
    if (area.isEmpty()) return;
    QPainter painter(this);
    for (const auto& [label, box] : boxes) {
        const QColor color = label.startsWith(QStringLiteral("顔")) ? QColor(QStringLiteral("#e03131")) : QColor(QStringLiteral("#1971c2"));
        painter.setPen(QPen(color, 2));
        const QRectF rect(area.x() + box[0] * area.width(), area.y() + box[1] * area.height(), box[2] * area.width(), box[3] * area.height());
        painter.drawRect(rect);
        painter.drawText(rect.topLeft() + QPointF(3, 12), label);
    }
    if (start_ && end_) {
        painter.setPen(QPen(QColor(QStringLiteral("#f08c00")), 2, Qt::DashLine));
        painter.drawRect(QRectF(*start_, *end_).normalized());
    }
}

PanelView::PanelView(MainWindow* window) : window_(window) {
    mode = new QComboBox;
    for (const auto& [label, key] : {std::pair{"採用中（印刷）", "print"}, {"採用中（校正）", "proof"}, {"比較", "compare"}, {"元画像", "source"}}) {
        mode->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    }
    connect(mode, &QComboBox::currentIndexChanged, this, [this](int) { show_image(); });
    image = new RegionImage;
    connect(image, &RegionImage::drawn, this, &PanelView::region_drawn);
    info = new QLabel;
    info->setWordWrap(true);
    candidates = new QListWidget;
    candidates->setViewMode(QListWidget::IconMode);
    candidates->setIconSize(QSize(90, 90));
    candidates->setMaximumHeight(130);
    connect(candidates, &QListWidget::currentRowChanged, this, [this](int) { candidate_selected(); });
    provenance = new QTextEdit;
    provenance->setReadOnly(true);
    provenance->setMaximumHeight(70);
    auto* adopt_button = new QPushButton(QStringLiteral("この候補を採用"));
    connect(adopt_button, &QPushButton::clicked, this, &PanelView::adopt);
    auto* big_button = new QPushButton(QStringLiteral("大きく見る"));
    connect(big_button, &QPushButton::clicked, this, &PanelView::open_viewer);
    auto* compare = new QPushButton(QStringLiteral("候補を並べて比べる"));
    connect(compare, &QPushButton::clicked, this, &PanelView::compare_candidates);
    instruction = new ReplyEdit;
    instruction->setObjectName(QStringLiteral("panelInstruction"));
    instruction->setPlaceholderText(QStringLiteral("このコマへの指示（AI の作業に出る）"));
    connect(instruction, &ReplyEdit::sendRequested, this, &PanelView::send_instruction);
    send_button = new QPushButton(QStringLiteral("指示を送る"));
    connect(send_button, &QPushButton::clicked, this, &PanelView::send_instruction);
    region_kind = new QComboBox;
    for (const auto& [key, label] : wording::regions()) region_kind->addItem(label, key);
    region_button = new QPushButton(QStringLiteral("領域を描く"));
    region_button->setCheckable(true);
    connect(region_button, &QPushButton::toggled, this, &PanelView::region_mode);
    regions = new QListWidget;
    regions->setMaximumHeight(60);
    auto* drop = new QPushButton(QStringLiteral("選んだ領域を消す"));
    connect(drop, &QPushButton::clicked, this, &PanelView::delete_region);
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(QStringLiteral("表示")));
    top->addWidget(mode, 1);
    top->addWidget(big_button);
    auto* row = new QHBoxLayout;
    row->addWidget(instruction, 1);
    row->addWidget(send_button, 0, Qt::AlignTop);
    auto* region_row = new QHBoxLayout;
    region_row->addWidget(region_kind);
    region_row->addWidget(region_button);
    region_row->addWidget(drop);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(image, 3);
    layout->addWidget(info);
    auto* cands_head = new QHBoxLayout;
    cands_head->addWidget(new QLabel(QStringLiteral("候補（選ぶと来歴が出る）")), 1);
    cands_head->addWidget(compare);
    layout->addLayout(cands_head);
    layout->addWidget(candidates);
    layout->addWidget(provenance);
    layout->addWidget(adopt_button);
    layout->addLayout(row);
    layout->addWidget(new QLabel(QStringLiteral("領域（顔・人物の位置、空けておく場所）")));
    layout->addLayout(region_row);
    layout->addWidget(regions);
}

const core::Frame* PanelView::frame() const {
    const core::Page* page = window_->current_page();
    if (page == nullptr || !frame_id) return nullptr;
    return page->find_frame(*frame_id);
}

void PanelView::set_frame(const std::optional<std::string>& id) {
    frame_id = id;
    refresh();
}

void PanelView::refresh() {
    const core::Frame* f = frame();
    candidates->clear();
    regions->clear();
    instruction->bind(draft_book_key(*window_), f != nullptr ? QStringLiteral("panel:") + qs(f->id) : QString());
    if (f == nullptr) {
        info->setText(QStringLiteral("編集画面でコマをクリックすると、そのコマの絵と候補がここに出ます。"));
        image->clear();
        image->boxes.clear();
        return;
    }
    const Json panel = f->panel && f->panel->is_object() ? *f->panel : Json::object();
    const Json adopted = at(panel, "adopted");
    const Json attempts = at(panel, "attempts");
    const QString brief = brief_words(panel).join(QStringLiteral(" / "));
    const Json human = at(panel, "instruction");
    const core::Page* page = window_->current_page();
    QString order = QStringLiteral("?");
    if (page != nullptr) {
        const auto leaves = page->leaf_frames();
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            if (leaves[i]->id == f->id) order = QString::number(i + 1);
        }
    }
    const Json images = attempts.is_object() && attempts.contains("images") ? attempts["images"] : Json(0);
    QString text = QStringLiteral("<b>%1 コマ目</b>　%2　取り込んだ絵 %3 枚<br>%4")
                       .arg(order, wording::panel_status(at(panel, "status")), qs(core::py_str(images)), brief.toHtmlEscaped());
    if (human.is_object() && core::py_truthy(at(human, "text"))) {
        text += QStringLiteral("<br>指示: %1").arg(qs(core::py_str(at(human, "text"))).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    }
    info->setText(text);
    std::vector<Json> adopted_ids;
    if (adopted.is_object()) {
        for (const auto& [key, value] : adopted.items()) {
            (void)key;
            adopted_ids.push_back(value);
        }
    }
    const Json cands = at(panel, "candidates");
    std::size_t n = 0;
    for (const Json& cand : core::py_truthy(cands) ? core::iterate(cands) : std::vector<Json>{}) {
        ++n;
        const Json review = at(cand, "review");
        const Json score = core::py_truthy(review) ? at(review, "score") : Json();
        const Json id = at(cand, "id");
        const bool is_adopted = std::any_of(adopted_ids.begin(), adopted_ids.end(), [&](const Json& v) { return core::py_equals(v, id); });
        auto* entry = new QListWidgetItem(QStringLiteral("候補 %1").arg(n) + (is_adopted ? QStringLiteral(" ✔採用") : QString()) +
                                          (score.is_null() ? QString() : QStringLiteral("\n評価 %1").arg(qs(core::py_str(score)))));
        entry->setToolTip(qs(core::py_str(id)));
        entry->setData(Qt::UserRole, qs(core::py_str(id)));
        if (const auto picture = asset_image(window_->session().path(), core::py_str(at(cand, "asset")))) {
            entry->setIcon(still_icon(QPixmap::fromImage(thumbnail(*picture, QSize(110, 110)))));
        }
        candidates->addItem(entry);
    }
    const Json listed = at(panel, "regions");
    for (const Json& region : core::py_truthy(listed) ? core::iterate(listed) : std::vector<Json>{}) {
        const QString who = at(region, "source") == Json("user") ? QStringLiteral("人") : QStringLiteral("AI");
        const Json chr = at(region, "char");
        auto* entry = new QListWidgetItem(QStringLiteral("%1 %2（%3）").arg(wording::region_label(at(region, "kind")),
                                                                         core::py_truthy(chr) ? qs(core::py_str(chr)) : QString(), who));
        entry->setData(Qt::UserRole, QVariant::fromValue(qs(at(region, "id").dump())));
        regions->addItem(entry);
    }
    show_image();
}

std::optional<Json> PanelView::selected_candidate() const {
    const core::Frame* f = frame();
    QListWidgetItem* entry = candidates->currentItem();
    if (f == nullptr || entry == nullptr || !f->panel) return std::nullopt;
    const QString id = entry->data(Qt::UserRole).toString();
    const Json cands = at(*f->panel, "candidates");
    for (const Json& cand : core::py_truthy(cands) ? core::iterate(cands) : std::vector<Json>{}) {
        if (qs(core::py_str(at(cand, "id"))) == id) return cand;
    }
    return std::nullopt;
}

void PanelView::candidate_selected() {
    const auto cand = selected_candidate();
    if (!cand) {
        provenance->clear();
        return;
    }
    const Json origin = at(*cand, "origin");
    const Json review = at(*cand, "review");
    const auto text = [&](const Json& value) { return core::py_truthy(value) ? qs(core::py_str(value)) : QString(); };
    QString maker = text(at(origin, "tool_id"));
    if (maker.isEmpty()) maker = text(at(origin, "kind"));
    if (maker.isEmpty()) maker = QStringLiteral("不明");
    const QString parent = text(at(*cand, "parent"));
    QStringList lines = {QStringLiteral("作った道具: %1 %2").arg(maker, text(at(origin, "model"))),
                         QStringLiteral("id: %1　元にした候補: %2　%3")
                             .arg(qs(core::py_str(at(*cand, "id"))), parent.isEmpty() ? QStringLiteral("なし") : parent,
                                  core::py_truthy(at(*cand, "stale")) ? QStringLiteral("（指示が変わる前の候補）") : QString())};
    if (core::py_truthy(at(origin, "prompt"))) lines << QStringLiteral("プロンプト: %1").arg(text(at(origin, "prompt")));
    if (core::py_truthy(review)) {
        lines << QStringLiteral("評価（%1）: %2 %3").arg(wording::actor(core::py_truthy(at(review, "by")) ? core::py_str(at(review, "by")) : std::string()),
                                                        qs(core::py_str(at(review, "score"))), text(at(review, "note")));
    }
    provenance->setPlainText(lines.join(QLatin1Char('\n')));
    const QString m = mode->currentData().toString();
    if (m == QLatin1String("compare") || m == QLatin1String("source")) show_image();
}

void PanelView::show_image() {
    const core::Frame* f = frame();
    const core::Page* page = window_->current_page();
    if (f == nullptr || page == nullptr) return;
    const QString m = mode->currentData().toString();
    std::optional<QImage> picture;
    bool frame_boxes = false;
    std::optional<Json> cand = selected_candidate();
    const Json panel = f->panel && f->panel->is_object() ? *f->panel : Json::object();
    if (!cand) {
        const Json art = at(at(panel, "adopted"), "art");
        const Json cands = at(panel, "candidates");
        for (const Json& c : core::py_truthy(cands) ? core::iterate(cands) : std::vector<Json>{}) {
            if (core::py_equals(at(c, "id"), art)) {
                cand = c;
                break;
            }
        }
    }
    try {
        if (m == QLatin1String("print") || m == QLatin1String("proof")) {
            render::RenderOptions options;
            options.mode = m.toStdString();
            options.skip_unported = true;
            picture = to_qimage(render::render_frame(*page, f->id, 150, options, &window_->book()));
            frame_boxes = true;
        } else if (m == QLatin1String("compare") && cand && window_->session().path()) {
            const auto [png, label] = service::render_kind(window_->book(), *window_->session().path(), page->index.json(), Json(f->id), Json("compare"),
                                                            at(*cand, "id"), Json(480));
            (void)label;
            QImage made = QImage::fromData(reinterpret_cast<const uchar*>(png.data()), static_cast<int>(png.size()));
            if (!made.isNull()) picture = made;
        } else if (m == QLatin1String("source") && cand) {
            picture = asset_image(window_->session().path(), core::py_str(at(*cand, "asset")));
        }
    } catch (const std::exception&) {
        picture.reset();  // (Python: an ApplyError leaves no picture)
    }
    if (!picture) {
        image->setText(QStringLiteral("（画像なし）"));
        image->boxes.clear();
        return;
    }
    const QPixmap pix = QPixmap::fromImage(*picture).scaled(std::max(320, image->width()), std::max(240, image->height()), Qt::KeepAspectRatio,
                                                            Qt::SmoothTransformation);
    image->setPixmap(pix);
    image->boxes = frame_boxes ? region_boxes(*f) : decltype(image->boxes){};
    image->update();
}

std::vector<std::pair<QString, std::array<double, 4>>> PanelView::region_boxes(const core::Frame& f) const {
    const core::Rect& r = f.rect;
    std::vector<std::pair<QString, std::array<double, 4>>> out;
    const Json listed = f.panel ? at(*f.panel, "regions") : Json();
    for (const Json& region : core::py_truthy(listed) ? core::iterate(listed) : std::vector<Json>{}) {
        const Json rect = at(region, "rect_mm");
        if (!rect.is_array() || rect.size() != 4) continue;
        try {
            const double x = core::py_float(rect[0]), y = core::py_float(rect[1]), w = core::py_float(rect[2]), h = core::py_float(rect[3]);
            const Json chr = at(region, "char");
            const QString label = QStringLiteral("%1 %2").arg(wording::region_label(at(region, "kind")), core::py_truthy(chr) ? qs(core::py_str(chr)) : QString()).trimmed();
            out.push_back({label, {(x - r.x.value()) / r.width.value(), (y - r.y.value()) / r.height.value(), w / r.width.value(), h / r.height.value()}});
        } catch (const core::Error&) {
        }
    }
    return out;
}

void PanelView::open_viewer() {
    const core::Frame* f = frame();
    const core::Page* page = window_->current_page();
    if (f == nullptr || page == nullptr) return;
    const auto cand = selected_candidate();
    const QString m = mode->currentData().toString();
    std::optional<QImage> picture;
    try {
        if (m == QLatin1String("print") || m == QLatin1String("proof")) {
            render::RenderOptions options;
            options.mode = m.toStdString();
            options.skip_unported = true;
            picture = to_qimage(render::render_frame(*page, f->id, 300, options, &window_->book()));
        } else if (cand && m == QLatin1String("compare") && window_->session().path()) {
            const auto [png, label] = service::render_kind(window_->book(), *window_->session().path(), page->index.json(), Json(f->id), Json("compare"),
                                                            at(*cand, "id"), Json(1200));
            (void)label;
            QImage made = QImage::fromData(reinterpret_cast<const uchar*>(png.data()), static_cast<int>(png.size()));
            if (!made.isNull()) picture = made;
        } else if (cand) {
            picture = asset_image(window_->session().path(), core::py_str(at(*cand, "asset")));
        }
    } catch (const std::exception& error) {
        window_->flash(wording::error(QString::fromUtf8(error.what())), 6000, true);
        return;
    }
    if (!picture) return;
    ViewerDialog dialog(this, QStringLiteral("コマ"), {{mode->currentText(), picture->convertToFormat(QImage::Format_RGB888)}});
    ask::exec(&dialog);
}

void PanelView::compare_candidates() {
    const core::Frame* f = frame();
    if (f == nullptr || !f->panel) return;
    std::vector<Json> adopted_ids;
    if (const Json adopted = at(*f->panel, "adopted"); adopted.is_object()) {
        for (const auto& [key, value] : adopted.items()) {
            (void)key;
            adopted_ids.push_back(value);
        }
    }
    std::vector<ViewerItem> shown;
    const Json cands = at(*f->panel, "candidates");
    std::size_t n = 0;
    for (const Json& cand : core::py_truthy(cands) ? core::iterate(cands) : std::vector<Json>{}) {
        ++n;
        const auto picture = asset_image(window_->session().path(), core::py_str(at(cand, "asset")));
        if (!picture) continue;
        const bool is_adopted = std::any_of(adopted_ids.begin(), adopted_ids.end(), [&](const Json& v) { return core::py_equals(v, at(cand, "id")); });
        shown.emplace_back(QStringLiteral("候補 %1").arg(n) + (is_adopted ? QStringLiteral(" ✔採用") : QString()), picture->convertToFormat(QImage::Format_RGB888));
    }
    if (shown.empty()) return;
    ViewerDialog dialog(this, QStringLiteral("候補を比べる"), shown, QStringLiteral("採用は「コマの詳細」で候補を選んで「この候補を採用」を押します。"));
    ask::exec(&dialog);
}

void PanelView::adopt() {
    const core::Page* page = window_->current_page();
    const auto cand = selected_candidate();
    if (page == nullptr || !cand || !frame_id) return;
    const std::string to = at(at(*cand, "origin"), "tool_id") == Json("genko:lineart") ? "ink" : "art";
    const Json ops = Json::array({Json{{"op", "adopt_candidate"}, {"page", page->index.json()}, {"frame_id", *frame_id}, {"candidate_id", at(*cand, "id")}, {"to", to}}});
    if (window_->apply_ops(ops)) emit changed();
}

void PanelView::send_instruction() {
    const core::Page* shown = window_->current_page();
    const QString text = QString::fromStdString(core::py_strip(instruction->toPlainText().toStdString()));
    if (shown == nullptr || !frame_id || text.isEmpty()) return;
    // (taken by value before the question: while it is asked the book may be read again — an AI's change — and the
    // page and the panel it held are gone then)
    const MainWindow::Asked asked = window_->asking();
    const std::string page_id = shown->id;
    const std::string panel = *frame_id;
    const QString book = draft_book_key(*window_);
    const QString item = QStringLiteral("panel:") + qs(panel);
    if (!confirm_send(this, QStringLiteral("指示を送る"), QString(), text)) return;  // (I13: asked before it goes; the draft stays)
    const core::Page* page = nullptr;
    if (window_->asking().session == asked.session) {
        for (const auto& candidate : window_->book().pages) {
            if (candidate->id == page_id && candidate->find_frame(panel) != nullptr) page = candidate.get();
        }
    }
    if (page == nullptr) {
        window_->flash(QStringLiteral("確認の間に原稿が変わり、そのコマが見つからなくなったため、指示を送っていません（書いた言葉は残っています）。"), 8000, true);
        return;
    }
    const std::string words = text.toStdString();
    const Json ops = Json::array({Json{{"op", "set_panel"},
                                       {"page", page->index.json()},
                                       {"frame_id", panel},
                                       {"set", Json{{"instruction", Json{{"text", words}, {"by", window_->session().actor()}}}}},
                                       {"pin", Json::array({"instruction"})}},
                                  Json{{"op", "request_fix"}, {"page", page->index.json()}, {"frame_id", panel}, {"instruction", words}, {"scope", "frame"}}});
    if (window_->apply_ops(ops)) {
        if (instruction->book() == book && instruction->item() == item) {
            instruction->sent();
        } else {
            Drafts::instance().forget(book, item);
        }
        emit changed();
    }
}

void PanelView::region_mode(bool on) {
    image->drawing = on;
    if (on) mode->setCurrentIndex(1);  // regions are drawn on the panel as it prints (proof)
}

void PanelView::region_drawn(double x, double y, double w, double h) {
    const core::Frame* f = frame();
    const core::Page* page = window_->current_page();
    if (f == nullptr || page == nullptr) return;
    const core::Rect& r = f->rect;
    const Json rect = Json::array({core::py_round(r.x.value() + x * r.width.value(), 2), core::py_round(r.y.value() + y * r.height.value(), 2),
                                   core::py_round(w * r.width.value(), 2), core::py_round(h * r.height.value(), 2)});
    region_button->setChecked(false);
    const Json ops = Json::array({Json{{"op", "add_region"},
                                       {"page", page->index.json()},
                                       {"frame_id", f->id},
                                       {"region", Json{{"kind", region_kind->currentData().toString().toStdString()}, {"rect_mm", rect}}}}});
    if (window_->apply_ops(ops)) emit changed();
}

void PanelView::delete_region() {
    const core::Page* page = window_->current_page();
    QListWidgetItem* entry = regions->currentItem();
    if (page == nullptr || entry == nullptr || !frame_id) return;
    Json id;
    try {
        id = core::parse_python_json(entry->data(Qt::UserRole).toString().toStdString());
    } catch (const core::Error&) {
        return;
    }
    if (window_->apply_ops(Json::array({Json{{"op", "delete_region"}, {"page", page->index.json()}, {"frame_id", *frame_id}, {"id", id}}}))) emit changed();
}

// --- library ---------------------------------------------------------------------------------------------------------

Library::Library(MainWindow* window) : window_(window) {
    list = new QListWidget;
    list->setViewMode(QListWidget::IconMode);
    list->setIconSize(QSize(96, 128));
    connect(list, &QListWidget::currentRowChanged, this, [this](int) { show_entry(); });
    detail = new QTextEdit;
    detail->setReadOnly(true);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QStringLiteral("キャラクターと背景")));
    layout->addWidget(list, 2);
    layout->addWidget(detail, 1);
}

void Library::refresh() {
    const core::Document& doc = window_->book();
    list->clear();
    entries_.clear();
    for (const Json& chr : core::py_truthy(doc.bible.characters) ? core::iterate(doc.bible.characters) : std::vector<Json>{}) {
        if (!chr.is_object()) continue;
        std::map<std::string, Json> refs;  // ({r.kind: r.asset}: the last of a kind wins)
        const Json listed = at(chr, "refs");
        for (const Json& r : core::py_truthy(listed) ? core::iterate(listed) : std::vector<Json>{}) refs[core::py_str(at(r, "kind"))] = at(r, "asset");
        const Json name = chr.contains("name") ? chr["name"] : at(chr, "id");
        const QString label = QStringLiteral("%1\n%2").arg(qs(core::py_str(name)), core::py_truthy(at(chr, "locked")) ? QStringLiteral("設定画 承認済み")
                                                                                                                  : QStringLiteral("設定画 未承認"));
        Json asset = refs.count("face") ? refs["face"] : Json();
        if (!core::py_truthy(asset)) asset = refs.count("sheet") ? refs["sheet"] : Json();
        Json data = Json{{"kind", "character"}};
        for (const auto& [key, value] : chr.items()) data[key] = value;
        add(label, core::py_truthy(asset) ? std::optional<std::string>(core::py_str(asset)) : std::nullopt, data);
    }
    const Json locations = at(doc.studio, "locations");
    for (const Json& loc : core::py_truthy(locations) ? core::iterate(locations) : std::vector<Json>{}) {
        if (!loc.is_object()) continue;
        Json asset;
        const Json listed = at(loc, "refs");
        for (const Json& r : core::py_truthy(listed) ? core::iterate(listed) : std::vector<Json>{}) {
            asset = at(r, "asset");
            break;
        }
        const Json name = core::py_truthy(at(loc, "name")) ? at(loc, "name") : at(loc, "id");
        Json data = Json{{"kind", "location"}};
        for (const auto& [key, value] : loc.items()) data[key] = value;
        add(QStringLiteral("%1\n場所").arg(qs(core::py_str(name))), core::py_truthy(asset) ? std::optional<std::string>(core::py_str(asset)) : std::nullopt, data);
    }
}

void Library::add(const QString& label, const std::optional<std::string>& asset, Json data) {
    auto* entry = new QListWidgetItem(label);
    if (asset) {
        if (const auto picture = asset_image(window_->session().path(), *asset)) entry->setIcon(still_icon(QPixmap::fromImage(thumbnail(*picture, QSize(96, 128)))));
    }
    list->addItem(entry);
    entries_.push_back(std::move(data));
}

void Library::show_entry() {
    const int row = list->currentRow();
    if (row < 0 || row >= static_cast<int>(entries_.size())) {
        detail->clear();
        return;
    }
    const Json& data = entries_[static_cast<std::size_t>(row)];
    const auto text = [](const Json& value) { return qs(core::py_str(value)); };
    const Json name = core::py_truthy(at(data, "name")) ? at(data, "name") : at(data, "id");
    QStringList lines = {QStringLiteral("%1: %2（%3）").arg(at(data, "kind") == Json("character") ? QStringLiteral("人物") : QStringLiteral("場所"), text(name),
                                                          text(at(data, "id")))};
    const Json look = at(data, "look");
    if (core::py_truthy(look) && look.is_object()) {
        QStringList parts;
        for (const auto& [key, value] : look.items()) {
            (void)key;
            if (value.is_string() && !value.get<std::string>().empty()) parts << qs(value.get<std::string>());
        }
        lines << QStringLiteral("見た目: ") + parts.join(QStringLiteral("、"));
    }
    if (core::py_truthy(at(data, "tokens_en"))) lines << QStringLiteral("画像ツール向けの見た目（tokens_en）: %1").arg(text(at(data, "tokens_en")));
    const Json refs = at(data, "refs");
    for (const Json& ref : core::py_truthy(refs) ? core::iterate(refs) : std::vector<Json>{}) {
        const Json kind = at(ref, "kind");
        const QString what = kind == Json("face") ? QStringLiteral("顔") : kind == Json("sheet") ? QStringLiteral("設定画") : text(kind);
        Json by = at(ref, "approved_by");
        if (!core::py_truthy(by)) by = at(ref, "by");
        lines << QStringLiteral("参照（%1）: 承認 %2").arg(what, wording::actor(core::py_truthy(by) ? core::py_str(by) : std::string()));
    }
    detail->setPlainText(lines.join(QLatin1Char('\n')));
}

}  // namespace genko::app
