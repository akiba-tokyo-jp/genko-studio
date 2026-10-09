#include "app/dialogs.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QScrollArea>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <system_error>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/icons.hpp"
#include "app/look.hpp"
#include "app/path_label.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/command_bus.hpp"
#include "core/error.hpp"
#include "core/ids.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "render/ops_registry.hpp"
#include "render/page.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"
#include "storage/reader.hpp"
#include "storage/transaction.hpp"
#include "storage/writer.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

QString qpath(const fs::path& path) { return QString::fromStdString(core::path_to_utf8(path)); }
fs::path fpath(const QString& text) { return core::path_from_utf8(text.toStdString()); }

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.mode() == "RGB" ? image : image.convert("RGB");
    const std::string bytes = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(bytes.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

// A spin box for millimetres (Python's PaperDialog.spin).
QDoubleSpinBox* mm_spin(double lo, double hi, double value) {
    auto* box = new QDoubleSpinBox;
    box->setRange(lo, hi);
    box->setDecimals(1);
    box->setSingleStep(0.5);
    box->setSuffix(QStringLiteral(" mm"));
    box->setValue(value);
    return box;
}

QHBoxLayout* pair(QWidget* a, QWidget* b) {
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    row->addWidget(a, 1);
    row->addWidget(new QLabel(QStringLiteral("×")));
    row->addWidget(b, 1);
    return row;
}

QString paper_error(const QString& message) {
    if (message == QLatin1String("the paper must hold the finished size and its bleed")) {
        return QStringLiteral("用紙が、仕上がりと裁ち落としより小さくなっています");
    }
    if (message == QLatin1String("the basic frame must fit inside the finished size")) return QStringLiteral("基本枠が仕上がりに収まりません");
    return wording::error(message);
}

// The spec without its preset name (Python compares dataclasses.replace(make(), preset=spec.preset) == spec).
Json spec_shape(const core::PageSpec& spec) {
    Json out = storage::spec_to_json(spec);
    out.erase("preset");
    return out;
}

// A recent book's title (its project.json, or the folder's name).
QString project_title(const fs::path& path) {
    try {
        const Json payload = core::parse_python_json(storage::read_file(path / "project.json"));
        if (payload.is_object() && payload.contains("title") && core::py_truthy(payload["title"])) return QString::fromStdString(core::py_str(payload["title"]));
    } catch (const std::exception&) {
    }
    return qpath(path.stem());
}

// "N ページ · 今日" under a recent book's cover (from project.json alone).
QString book_facts(const fs::path& path) {
    try {
        const Json payload = core::parse_python_json(storage::read_file(path / "project.json"));
        const std::size_t pages = payload.contains("pages") && payload["pages"].is_array() ? payload["pages"].size() : 0;
        const QDateTime when = QFileInfo(qpath(path / "project.json")).lastModified();
        const QDate today = QDate::currentDate();
        QString day;
        if (when.date() == today) {
            day = QStringLiteral("今日");
        } else if (when.date().daysTo(today) == 1) {
            day = QStringLiteral("昨日");
        } else {
            day = QStringLiteral("%1月%2日").arg(when.date().month()).arg(when.date().day());
        }
        return QStringLiteral("%1 ページ · %2").arg(pages).arg(day);
    } catch (const std::exception&) {
        return {};
    }
}

// The book's first page, small, kept in the cache folder until the book changes (Python's cover_thumbnail).
QImage cover_thumbnail(const fs::path& path, int height) {
    const QString source = qpath(path / "project.json");
    const QFileInfo info(source);
    if (!info.exists()) return {};
    const QString stamp = QString::number(info.lastModified().toSecsSinceEpoch());
    const QByteArray id = QCryptographicHash::hash(qpath(fs::absolute(path)).toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QString folder = qpath(cache_root() / "covers");
    const QString target = folder + QStringLiteral("/%1_%2.png").arg(QString::fromLatin1(id), stamp);
    if (!QFileInfo::exists(target)) {
        try {
            const storage::LoadResult loaded = storage::load_document(path);
            const core::Page& page = loaded.document.page(0);
            render::RenderOptions options;
            options.skip_unported = true;
            const int dpi = std::max(8, static_cast<int>(height / (page.spec.height_mm.value() / 25.4)));
            const QImage image = to_qimage(render::render_page(page, dpi, options, &loaded.document).image);
            QDir().mkpath(folder);
            for (const QString& old : QDir(folder).entryList({QString::fromLatin1(id) + QStringLiteral("_*.png")})) QFile::remove(folder + "/" + old);
            image.save(target);
        } catch (const std::exception&) {
            return {};  // (a book that cannot be read shows a plain card)
        }
    }
    const QImage image(target);
    return image.isNull() ? image : image.scaledToHeight(height, Qt::SmoothTransformation);
}

// A way to begin: its picture, its name and one line of what it does (Python's _ActionCard).
QPushButton* action_card(const char* icon, const QString& title, const QString& words, bool main) {
    auto* card = new QPushButton;
    card->setObjectName(QStringLiteral("actionCard"));
    card->setProperty("main", main);
    card->setCursor(Qt::PointingHandCursor);
    card->setAccessibleName(title);
    card->setToolTip(words);
    card->setMinimumHeight(144);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* mark = new QLabel;
    mark->setPixmap(icons::icon(icon).pixmap(28, 28));
    auto* name = new QLabel(title);
    theme::role(name, "heading");
    name->setWordWrap(true);
    auto* note = new QLabel(words);
    theme::role(note, "caption");
    note->setWordWrap(true);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(8);
    for (QWidget* widget : {static_cast<QWidget*>(mark), static_cast<QWidget*>(name), static_cast<QWidget*>(note)}) {
        widget->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(widget);
    }
    layout->addStretch(1);
    return card;
}

}  // namespace

// --- recent books -----------------------------------------------------------------------------------------------------

std::vector<fs::path> recent_projects() {
    std::vector<fs::path> out;
    try {
        const Json items = core::parse_python_json(storage::read_file(config_dir() / "recent.json"));
        if (!items.is_array()) return out;
        for (const Json& item : items) {
            if (!item.is_string()) continue;
            const fs::path path = core::path_from_utf8(item.get<std::string>());
            std::error_code ec;
            if (fs::is_regular_file(path / "project.json", ec)) out.push_back(path);
        }
    } catch (const std::exception&) {
    }
    return out;
}

void remember_project(const fs::path& path) {
    std::error_code ec;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(path, ec), ec);
    Json items = Json::array({core::path_to_utf8(absolute)});
    for (const fs::path& p : recent_projects()) {
        if (fs::weakly_canonical(p, ec) != absolute && items.size() < 12) items.push_back(core::path_to_utf8(p));
    }
    try {
        storage::make_dirs_durable(config_dir());
        storage::write_atomic(config_dir() / "recent.json", core::dump_python_indent2(items));
    } catch (const core::Error&) {
        // (the list of recent books is a convenience: not being able to keep it stops nothing)
    }
}

QString safe_name(const QString& text, const QString& fallback) {
    static const QString bad = QStringLiteral("<>:\"/\\|?*");
    QString out;
    for (const QChar c : text) out += (bad.contains(c) || c.unicode() < 32) ? QChar('_') : c;
    out = out.trimmed();
    while (!out.isEmpty() && (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))) out.chop(1);
    if (out.isEmpty()) out = fallback;
    static const QStringList reserved = [] {
        QStringList names{QStringLiteral("CON"), QStringLiteral("PRN"), QStringLiteral("AUX"), QStringLiteral("NUL")};
        for (int i = 1; i <= 9; ++i) names << QStringLiteral("COM%1").arg(i) << QStringLiteral("LPT%1").arg(i);
        return names;
    }();
    if (reserved.contains(out.section(QLatin1Char('.'), 0, 0).toUpper())) out = QStringLiteral("_") + out;
    return out.left(80);
}

// --- the paper ------------------------------------------------------------------------------------------------------

PaperDialog::PaperDialog(QWidget* parent, const core::PageSpec& spec, bool changing) : QDialog(parent) {
    setWindowTitle(QStringLiteral("原稿用紙の設定"));
    preset = new QComboBox;
    preset->addItem(QStringLiteral("（数値で決める）"), QString());
    for (const core::PaperPreset& p : core::paper_presets()) {
        preset->addItem(QString::fromUtf8(p.label.data(), static_cast<qsizetype>(p.label.size())), QString::fromLatin1(p.key.data(), static_cast<qsizetype>(p.key.size())));
    }
    const auto [tw, th] = spec.trim_size();
    const core::PageSpec::Margins m = spec.margins();
    paper_w = mm_spin(20, 1000, spec.width_mm.value());
    paper_h = mm_spin(20, 2000, spec.height_mm.value());
    trim_w = mm_spin(10, 1000, tw.value());
    trim_h = mm_spin(10, 2000, th.value());
    bleed = mm_spin(0, 20, spec.bleed_mm.value());
    top = mm_spin(0, 200, m.top);
    bottom = mm_spin(0, 200, m.bottom);
    inner = mm_spin(0, 200, m.inner);
    outer = mm_spin(0, 200, m.outer);
    dpi = new QSpinBox;
    dpi->setRange(72, 1200);
    dpi->setValue(static_cast<int>(spec.dpi.value()));
    dpi->setSuffix(QStringLiteral(" dpi"));
    QFormLayout* rows = look::form();
    rows->addRow(look::section(QStringLiteral("用紙と仕上がり")));
    rows->addRow(QStringLiteral("見本"), preset);
    rows->addRow(QStringLiteral("用紙（幅×高さ）"), pair(paper_w, paper_h));
    rows->addRow(QStringLiteral("仕上がり（幅×高さ）"), pair(trim_w, trim_h));
    rows->addRow(QStringLiteral("裁ち落とし"), bleed);
    rows->addRow(QStringLiteral("解像度"), dpi);
    rows->addRow(look::section(QStringLiteral("基本枠までの余白（仕上がりから）")));
    rows->addRow(QStringLiteral("上・下"), pair(top, bottom));
    rows->addRow(QStringLiteral("のど・小口"), pair(inner, outer));
    look::quiet_labels(rows);
    summary = new QLabel;
    summary->setWordWrap(true);
    theme::role(summary, "hint");
    move = new QCheckBox(QStringLiteral("コマ・台詞・絵を新しい基本枠に合わせて動かす"));
    move->setChecked(true);
    move->setVisible(changing);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    cancel_ = buttons->button(QDialogButtonBox::Cancel);
    ok_->setText(changing ? QStringLiteral("変える") : QStringLiteral("決める"));
    cancel_->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        try {
            (void)this->spec();
            accept();
        } catch (const core::Error&) {
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* body = new QVBoxLayout;
    body->setSpacing(8);
    body->addLayout(rows);
    body->addWidget(move);
    body->addStretch(1);
    diagram_ = new look::PaperDiagram;
    auto* side = new QVBoxLayout;
    side->addWidget(diagram_, 1);
    side->addWidget(look::legend());
    side->addWidget(summary);
    look::frame(this, look::header(QStringLiteral("原稿用紙の設定"),
                                   QStringLiteral("数値は出版社・印刷所で違います。投稿・入稿の前に、先方の原稿用紙の指定を確かめてください。")),
                body, look::card(side, QStringLiteral("できあがりの形")), look::footer(buttons));
    look::fit_to_screen(this, QSize(760, 520));
    for (QDoubleSpinBox* box : {paper_w, paper_h, trim_w, trim_h, bleed, top, bottom, inner, outer}) {
        connect(box, &QDoubleSpinBox::valueChanged, this, [this] { changed(); });
    }
    connect(dpi, &QSpinBox::valueChanged, this, [this] { changed(); });
    connect(preset, &QComboBox::currentIndexChanged, this, [this] { from_preset(); });
    // the book on a preset: shown as that
    for (const core::PaperPreset& p : core::paper_presets()) {
        if (spec_shape(p.make()) == spec_shape(spec)) {
            preset->blockSignals(true);
            preset->setCurrentIndex(preset->findData(QString::fromLatin1(p.key.data(), static_cast<qsizetype>(p.key.size()))));
            preset->blockSignals(false);
            preset_key_ = QString::fromLatin1(p.key.data(), static_cast<qsizetype>(p.key.size()));
            break;
        }
    }
    changed(true);
}

void PaperDialog::from_preset() {
    const QString key = preset->currentData().toString();
    if (key.isEmpty()) return;
    const core::PaperPreset* p = core::find_paper_preset(key.toStdString());
    if (p == nullptr) return;
    const core::PageSpec spec = p->make();
    const auto [tw, th] = spec.trim_size();
    const core::PageSpec::Margins m = spec.margins();
    const std::pair<QDoubleSpinBox*, double> values[] = {{paper_w, spec.width_mm.value()}, {paper_h, spec.height_mm.value()}, {trim_w, tw.value()},
                                                         {trim_h, th.value()}, {bleed, spec.bleed_mm.value()}, {top, m.top},
                                                         {bottom, m.bottom}, {inner, m.inner}, {outer, m.outer}};
    for (const auto& [box, value] : values) {
        box->blockSignals(true);
        box->setValue(value);
        box->blockSignals(false);
    }
    dpi->blockSignals(true);
    dpi->setValue(static_cast<int>(spec.dpi.value()));
    dpi->blockSignals(false);
    preset_key_ = key;
    changed(true);
}

core::PageSpec PaperDialog::spec() const {
    if (!preset_key_.isEmpty()) {
        if (const core::PaperPreset* p = core::find_paper_preset(preset_key_.toStdString())) return p->make();
    }
    return core::PageSpec::custom(paper_w->value(), paper_h->value(), trim_w->value(), trim_h->value(), bleed->value(), top->value(),
                                  bottom->value(), inner->value(), outer->value(), dpi->value());
}

Json PaperDialog::op() const {
    if (!preset_key_.isEmpty()) {
        return Json::object({{"op", "set_page_spec"}, {"preset", preset_key_.toStdString()}, {"move", move->isChecked()}});
    }
    return Json::object({{"op", "set_page_spec"},
                         {"paper", Json::array({paper_w->value(), paper_h->value()})},
                         {"trim", Json::array({trim_w->value(), trim_h->value()})},
                         {"bleed_mm", bleed->value()},
                         {"margins", Json::array({top->value(), bottom->value(), inner->value(), outer->value()})},
                         {"dpi", dpi->value()},
                         {"move", move->isChecked()}});
}

void PaperDialog::changed(bool keep_preset) {
    if (!keep_preset && !preset_key_.isEmpty()) {
        preset_key_.clear();
        preset->blockSignals(true);
        preset->setCurrentIndex(0);
        preset->blockSignals(false);
    }
    bool ok = true;
    QString text;
    std::optional<core::PageSpec> shown;
    try {
        shown = spec();
        text = QString::fromStdString(shown->describe());
    } catch (const core::Error& error) {
        text = paper_error(QString::fromUtf8(error.what()));
        ok = false;
    }
    summary->setText(text);
    theme::role(summary, ok ? "hint" : "error");
    if (ok) {
        diagram_->show_spec(*shown, false);
    } else if (diagram_->spec()) {
        diagram_->show_spec(*diagram_->spec(), true);
    }
    ok_->setEnabled(ok);
}

// --- a new book -------------------------------------------------------------------------------------------------------

NewProjectDialog::NewProjectDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("新しい原稿"));
    setMinimumWidth(std::min(520, 400));
    title = new QLineEdit;
    title->setPlaceholderText(QStringLiteral("例: 夏の午後の約束"));
    episode = new QSpinBox;
    episode->setRange(1, 999);
    pages = new QSpinBox;
    pages->setRange(1, 400);
    pages->setValue(16);
    paper = new QComboBox;
    for (const core::PaperPreset& p : core::paper_presets()) {
        paper->addItem(QString::fromUtf8(p.label.data(), static_cast<qsizetype>(p.label.size())), QString::fromLatin1(p.key.data(), static_cast<qsizetype>(p.key.size())));
    }
    paper->addItem(QStringLiteral("自分で決める…"), QStringLiteral("custom"));
    const QString wanted = settings()->value(QStringLiteral("new/paper"), QString()).toString();  // (環境設定の用紙)
    if (!wanted.isEmpty() && paper->findData(wanted) >= 0) paper->setCurrentIndex(paper->findData(wanted));
    paper_note_ = new QLabel;
    paper_note_->setWordWrap(true);
    theme::role(paper_note_, "hint");
    binding = new QComboBox;
    binding->addItem(QStringLiteral("右綴じ（縦書きの漫画）"), QStringLiteral("right"));
    binding->addItem(QStringLiteral("左綴じ"), QStringLiteral("left"));
    folder = new QLineEdit(QDir::homePath());
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString chosen = ask::existing_dir(this, QStringLiteral("保存する場所"), folder->text());
        if (!chosen.isEmpty()) folder->setText(chosen);
    });
    auto* where = new QHBoxLayout;
    where->addWidget(folder, 1);
    where->addWidget(pick);
    where_note_ = new PathLabel;
    theme::role(where_note_, "hint");
    connect(title, &QLineEdit::textChanged, this, [this] { note(); });
    connect(folder, &QLineEdit::textChanged, this, [this] { note(); });
    connect(episode, &QSpinBox::valueChanged, this, [this] { note(); });
    QFormLayout* book = look::form();
    book->addRow(look::section(QStringLiteral("作品")));
    book->addRow(QStringLiteral("作品名"), title);
    auto* counts = new QHBoxLayout;
    counts->addWidget(episode, 1);
    counts->addWidget(new QLabel(QStringLiteral("話　")));
    counts->addWidget(pages, 1);
    counts->addWidget(new QLabel(QStringLiteral("ページ")));
    book->addRow(QStringLiteral("話数・ページ数"), counts);
    book->addRow(QStringLiteral("綴じ"), binding);
    book->addRow(look::section(QStringLiteral("原稿用紙")));
    book->addRow(QStringLiteral("原稿用紙"), paper);
    book->addRow(QString(), paper_note_);
    book->addRow(look::section(QStringLiteral("保存")));
    book->addRow(QStringLiteral("保存する場所"), where);
    book->addRow(QString(), where_note_);
    look::quiet_labels(book);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    cancel_ = buttons->button(QDialogButtonBox::Cancel);
    ok_->setText(QStringLiteral("作る"));
    cancel_->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { create(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* body = new QVBoxLayout;
    body->addLayout(book);
    body->addStretch(1);
    diagram_ = new look::PaperDiagram;
    auto* side = new QVBoxLayout;
    side->addWidget(diagram_, 1);
    side->addWidget(look::legend());
    look::frame(this, look::header(QStringLiteral("新しい原稿"), QStringLiteral("あとから「ページ → 原稿用紙の設定」で、用紙もページ数も変えられます。")),
                body, look::card(side, QStringLiteral("原稿用紙")), look::footer(buttons));
    look::fit_to_screen(this, QSize(780, 500));
    note();
    paper_changed();
    connect(paper, &QComboBox::currentIndexChanged, this, [this] { paper_changed(); });
}

fs::path NewProjectDialog::target() const {
    QString name = safe_name(title->text().trimmed().isEmpty() ? QStringLiteral("無題") : title->text().trimmed(), QStringLiteral("manga"));
    if (episode->value() > 1) name += QStringLiteral("_%1").arg(episode->value(), 2, 10, QLatin1Char('0'));
    QString base = folder->text();
    if (base.startsWith(QLatin1Char('~'))) base = QDir::homePath() + base.mid(1);
    return fpath(base) / fpath(name + QStringLiteral(".genko"));
}

void NewProjectDialog::note() { where_note_->set_path(qpath(target()), QStringLiteral("作られるフォルダ: ")); }

core::PageSpec NewProjectDialog::chosen_spec() const {
    if (paper->currentData().toString() == QLatin1String("custom")) return custom_spec_.value_or(core::PageSpec::b4_comic());
    if (const core::PaperPreset* p = core::find_paper_preset(paper->currentData().toString().toStdString())) return p->make();
    return core::PageSpec::a4_mono();
}

void NewProjectDialog::paper_changed() {
    if (paper->currentData().toString() == QLatin1String("custom")) {
        PaperDialog dialog(this, custom_spec_.value_or(core::PageSpec::b4_comic()));
        if (ask::exec(&dialog) == QDialog::Accepted) {
            custom_spec_ = dialog.spec();
        } else if (!custom_spec_) {
            paper->setCurrentIndex(0);
            return;
        }
    }
    paper_note_->setText(QString::fromStdString(chosen_spec().describe()));
    diagram_->show_spec(chosen_spec());
}

bool NewProjectDialog::create() {
    const fs::path where = target();
    std::error_code ec;
    if (fs::exists(where / "project.json", ec)) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("同じ名前の原稿がすでにあります:\n%1").arg(QDir::toNativeSeparators(qpath(where))));
        return false;
    }
    core::Document doc = core::new_episode(title->text().trimmed().isEmpty() ? std::string("無題") : title->text().trimmed().toStdString(),
                                           core::Num(static_cast<std::int64_t>(episode->value())), pages->value(), chosen_spec(),
                                           binding->currentData().toString() == QLatin1String("left") ? core::Binding::Left : core::Binding::Right);
    const std::string actor = default_actor();
    // (written on a worker thread: the window keeps answering)
    QFutureWatcher<QString> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<QString>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([doc = std::move(doc), where, actor]() -> QString {
        try {
            storage::ProjectLock lock(where, actor);
            lock.try_acquire();
            storage::SaveRequest request;
            request.actor = actor;
            request.ops = Json::array();
            storage::Saver(lock).save(doc, request);
            return {};
        } catch (const std::exception& error) {
            return QString::fromUtf8(error.what());
        }
    }));
    if (!watcher.isFinished()) loop.exec();
    const QString failed = watcher.result();
    if (!failed.isEmpty()) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("保存できませんでした:\n%1").arg(failed));
        return false;
    }
    created = where;
    accept();
    return true;
}

// --- the start screen -------------------------------------------------------------------------------------------------

namespace {

// A recent book: its first page on a sheet, its title and its facts under it (Python's _CoverCard).
class CoverCard : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const theme::Tokens t = theme::tokens();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect box = option.rect.adjusted(6, 6, -6, -6);
        const bool chosen = (option.state & QStyle::State_Selected) != 0;
        const bool hover = (option.state & QStyle::State_MouseOver) != 0;
        if (chosen || hover) {
            painter->setPen(QPen(QColor(chosen ? t.accent : t.border), 1.5));
            painter->setBrush(QColor(chosen ? t.selected : t.hover));
            painter->drawRoundedRect(box, 10, 10);
        }
        const QSize size(132, 180);
        const QRect sheet(box.center().x() - size.width() / 2, box.top() + 12, size.width(), size.height());
        painter->setPen(QPen(QColor(t.divider), 1));
        painter->setBrush(QColor(t.base));
        painter->drawRoundedRect(sheet.adjusted(-1, -1, 1, 1), 3, 3);
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            const QPixmap pixmap = icon.pixmap(size);
            QRect target(0, 0, pixmap.width(), pixmap.height());
            target.moveCenter(sheet.center());
            painter->drawPixmap(target, pixmap);
        }
        const QRect text_box(box.left() + 8, sheet.bottom() + 10, box.width() - 16, 20);
        QFont font = option.font;
        font.setWeight(QFont::Medium);
        painter->setFont(font);
        painter->setPen(QColor(t.text));
        painter->drawText(text_box, Qt::AlignHCenter | Qt::AlignTop,
                          painter->fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text_box.width()));
        painter->setFont(option.font);
        painter->setPen(QColor(t.muted));
        painter->drawText(text_box.translated(0, 20), Qt::AlignHCenter | Qt::AlignTop, index.data(Qt::UserRole + 1).toString());
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(172, 262); }
};

}  // namespace

StartDialog::StartDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("Genko Studio"));
    auto* name = new QLabel(QStringLiteral("Genko Studio"));
    theme::role(name, "title");
    auto* lead = new QLabel(QStringLiteral("漫画の原稿を作るアプリです。自分で描くことも、AI に描いてもらって確かめ・直すこともできます。"));
    theme::role(lead, "hint");
    lead->setWordWrap(true);
    notice_ = new QLabel;
    notice_->setWordWrap(true);
    notice_->setObjectName(QStringLiteral("startNotice"));
    notice_->hide();
    QPushButton* new_card = action_card("page", QStringLiteral("新しい原稿を作る"), QStringLiteral("用紙とページ数を決めて、白い原稿から自分で描きます"), true);
    QPushButton* open_card = action_card("open", QStringLiteral("原稿を開く"), QStringLiteral("このパソコンにある .genko の原稿を開きます"), false);
    QPushButton* ai_card = action_card("wand", QStringLiteral("AI と作る"),
                                       QStringLiteral("Claude などの AI をつなぎ、作りたい話を伝えて、届いた依頼を確かめます"), false);
    cards_ = {new_card, open_card, ai_card};
    connect(new_card, &QPushButton::clicked, this, [this] {
        NewProjectDialog dialog(this);
        if (ask::exec(&dialog) == QDialog::Accepted && dialog.created) pick(*dialog.created);
    });
    connect(open_card, &QPushButton::clicked, this, [this] {
        const QString path = ask::existing_dir(this, QStringLiteral("原稿（.genko のフォルダ）を開く"));
        if (!path.isEmpty()) pick(fpath(path));
    });
    connect(ai_card, &QPushButton::clicked, this, [this] {
        // (the AI connection's screens come with M5: said, not hidden)
        ask::warning(this, QStringLiteral("AI と作る"), QStringLiteral("この版の Genko では、AI とつなぐ画面はまだ使えません（次の段階で入ります）。"));
    });
    auto* cards = new QHBoxLayout;
    cards->setSpacing(16);
    for (QPushButton* card : cards_) cards->addWidget(card, 1);
    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("recentBooks"));
    list_->setViewMode(QListWidget::IconMode);
    list_->setIconSize(QSize(132, 180));
    list_->setGridSize(QSize(176, 266));
    list_->setResizeMode(QListWidget::Adjust);
    list_->setMovement(QListWidget::Static);
    list_->setMouseTracking(true);
    list_->setItemDelegate(new CoverCard(list_));
    paths_ = recent_projects();
    QPixmap blank(QSize(132, 180));
    blank.fill(QColor(theme::tokens().base));
    for (const fs::path& path : paths_) {
        auto* item = new QListWidgetItem(QIcon(blank), project_title(path));
        item->setData(Qt::UserRole, qpath(path));
        item->setData(Qt::UserRole + 1, book_facts(path));
        item->setToolTip(QDir::toNativeSeparators(qpath(path)));
        list_->addItem(item);
    }
    if (list_->count() > 0) list_->setCurrentRow(0);
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { pick(fpath(item->data(Qt::UserRole).toString())); });
    auto* empty = new QLabel(QStringLiteral("最近開いた原稿は、ここに 1 ページ目の絵で並びます。"));
    theme::role(empty, "caption");
    empty->setVisible(list_->count() == 0);
    list_->setVisible(list_->count() > 0);
    open_ = new QPushButton(QStringLiteral("開く"));
    theme::primary(open_);
    open_->setToolTip(QStringLiteral("選んだ原稿を開きます（ダブルクリックでも開きます）"));
    open_->setDefault(true);
    open_->setVisible(list_->count() > 0);
    connect(open_, &QPushButton::clicked, this, [this] {
        if (list_->currentItem() != nullptr) pick(fpath(list_->currentItem()->data(Qt::UserRole).toString()));
    });
    auto* recent_head = new QHBoxLayout;
    auto* heading = new QLabel(QStringLiteral("最近の原稿"));
    theme::role(heading, "heading");
    recent_head->addWidget(heading);
    recent_head->addStretch(1);
    recent_head->addWidget(open_);
    // everything scrolls on a small screen; the close button under it stays in sight
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(name);
    layout->addWidget(lead);
    layout->addWidget(notice_);
    layout->addSpacing(16);
    layout->addLayout(cards);
    layout->addSpacing(24);
    layout->addLayout(recent_head);
    list_->setMinimumHeight(list_->count() > 0 ? 280 : 0);
    layout->addWidget(list_, 1);
    layout->addWidget(empty);
    layout->addStretch(list_->count() > 0 ? 0 : 1);
    auto* scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("dialogBody"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    auto* buttons = new QDialogButtonBox;
    close_ = buttons->addButton(QStringLiteral("閉じる"), QDialogButtonBox::RejectRole);
    close_->setToolTip(QStringLiteral("原稿を開かずに Genko を終わります"));
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(32, 32, 32, 24);
    outer->setSpacing(8);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    look::fit_to_screen(this, QSize(960, list_->count() > 0 ? 640 : 440));  // (no recent books: no empty half screen)
    // the recent books' first pages, one at a time on a worker thread (the window opens at once)
    for (int row = 0; row < list_->count(); ++row) {
        const fs::path path = paths_[static_cast<std::size_t>(row)];
        auto* watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, row] {
            const QImage cover = watcher->result();
            watcher->deleteLater();
            if (cover.isNull() || row >= list_->count()) return;
            QIcon picture;
            for (const auto mode : {QIcon::Normal, QIcon::Selected, QIcon::Active}) picture.addPixmap(QPixmap::fromImage(cover), mode);
            list_->item(row)->setIcon(picture);
        });
        watcher->setFuture(QtConcurrent::run([path] { return cover_thumbnail(path, 180); }));
    }
}

void StartDialog::pick(const fs::path& path) {
    chosen = path;
    accept();
}

void StartDialog::show_notice(const QString& words) {
    notice_->setText(words);
    notice_->setVisible(!words.isEmpty());
}

// --- panel templates -----------------------------------------------------------------------------------------------------

TemplateDialog::TemplateDialog(QWidget* parent, std::shared_ptr<const core::Document> doc, std::size_t page_index, std::string actor)
    : QDialog(parent),
      doc_(std::move(doc)),
      page_index_(page_index),
      actor_(std::move(actor)),
      pool_(std::make_unique<QThreadPool>()),
      self_(std::make_shared<TemplateDialog*>(this)) {
    setWindowTitle(QStringLiteral("テンプレートでコマを割る"));
    pool_->setMaxThreadCount(2);
    const core::Page& page = doc_->page(page_index_);
    needs_clearing = !templates::is_blank(*doc_, page);
    list_ = new QListWidget;
    list_->setViewMode(QListWidget::IconMode);
    list_->setIconSize(QSize(120, 170));
    list_->setResizeMode(QListWidget::Adjust);
    list_->setSpacing(8);
    list_->setWordWrap(true);
    for (const auto& t : templates::mine()) items_.push_back(t);  // (the person's own layouts first: 自分のコマ割り)
    for (const auto& t : templates::builtin()) items_.push_back(t);
    QPixmap blank(QSize(120, 170));
    blank.fill(QColor(QStringLiteral("#f4f4f4")));
    for (std::size_t i = 0; i < items_.size(); ++i) {
        auto* item = new QListWidgetItem(QIcon(blank), items_[i].label);
        item->setData(Qt::UserRole, static_cast<int>(i));
        list_->addItem(item);
        // its picture: the page cut by it, drawn small on a worker thread
        ++pending_;
        std::weak_ptr<TemplateDialog*> self = self_;
        const auto chosen = items_[i];
        const auto book = doc_;
        const std::size_t index = page_index_;
        const std::string who = actor_;
        pool_->start([self, chosen, book, index, who, i]() {
            QImage picture;
            try {
                const templates::Plan p = templates::plan(*book, index, chosen, who);
                core::ScopedIdScript script(p.ids);
                const core::ApplyResult result = core::CommandBus(render::ops_registry()).apply(*book, p.ops, core::Actor(who));
                render::RenderOptions options;
                options.skip_unported = true;
                picture = to_qimage(render::render_page(result.doc.page(index), 20, options, &result.doc).image);
            } catch (const std::exception&) {
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [self, picture, i]() {
                    if (const auto alive = self.lock()) {
                        TemplateDialog* dialog = *alive;
                        --dialog->pending_;
                        if (!picture.isNull() && static_cast<int>(i) < dialog->list_->count()) {
                            dialog->list_->item(static_cast<int>(i))->setIcon(QIcon(QPixmap::fromImage(picture)));
                        }
                    }
                },
                Qt::QueuedConnection);
        });
    }
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { choose(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        // right click on one of the person's own layouts: delete it
        QListWidgetItem* item = list_->itemAt(pos);
        if (item == nullptr) return;
        const auto& t = items_[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())];
        if (!t.mine) return;
        QMenu menu(this);
        QAction* gone = menu.addAction(QStringLiteral("このテンプレートを消す"));
        if (menu.exec(list_->viewport()->mapToGlobal(pos)) == gone) {
            templates::remove_mine(QString::fromStdString(t.key));
            delete list_->takeItem(list_->row(item));
        }
    });
    auto* note = new QLabel(needs_clearing ? QStringLiteral("コマと台詞は作り直されます（元に戻す で取り消せます）。") : QStringLiteral("空のページをテンプレートで割ります。"));
    note->setWordWrap(true);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("このテンプレートで割る"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { choose(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(list_, 1);
    layout->addWidget(note);
    layout->addWidget(buttons);
    look::fit_to_screen(this, QSize(720, 520));
}

TemplateDialog::~TemplateDialog() {
    self_.reset();
    pool_->clear();
    pool_->waitForDone();
}

bool TemplateDialog::wait_pictures(int ms) {
    QElapsedTimer clock;
    clock.start();
    while (pending_ > 0) {
        if (clock.elapsed() > ms) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

void TemplateDialog::choose() {
    QListWidgetItem* item = list_->currentItem();
    if (item == nullptr) return;
    const auto& t = items_[static_cast<std::size_t>(item->data(Qt::UserRole).toInt())];
    try {
        plan = templates::plan(*doc_, page_index_, t, actor_);
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    accept();
}

}  // namespace genko::app
