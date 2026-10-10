// 書き出し… (Python's genko/app/dialogs.py ExportDialog, icc_setting, set_icc_setting).

#include "app/export_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <set>
#include <tuple>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/look.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
#include "core/error.hpp"
#include "core/paths.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "formats/checks.hpp"
#include "formats/export.hpp"
#include "formats/psd_text.hpp"
#include "render/colour.hpp"
#include "render/page.hpp"

namespace genko::app {

namespace fs = std::filesystem;
using core::Json;

namespace {

QString qs(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// dialogs.SCREEN_SHAPES
const std::vector<std::pair<const char*, const char*>>& screen_shapes() {
    static const std::vector<std::pair<const char*, const char*>> list = {{"丸", "round"}, {"四角", "square"}, {"ひし形", "diamond"}, {"楕円", "ellipse"}};
    return list;
}

// export.AREA_LABELS
QString area_label(std::string_view key) {
    if (key == "paper") return QStringLiteral("用紙全体（トンボ付き）");
    if (key == "bleed") return QStringLiteral("裁ち落としまで（入稿の標準）");
    return QStringLiteral("仕上がりまで");
}

// (PSD-01) A feature the editable PSD cannot keep in words (formats::psd_text_unsupported).
QString feature_words(const std::vector<std::string>& keys) {
    QStringList words;
    for (const std::string& key : keys) {
        QString word = qs(key);
        for (const formats::PsdTextFeature& f : formats::psd_text_unsupported()) {
            if (f.key == key) word = qs(f.label);
        }
        words << word;
    }
    return words.join(QStringLiteral("、"));
}

// (PSD-01) The lines the editable PSD cannot keep as text, one a line (the first ten).
QString issue_lines(const std::vector<formats::PsdTextIssue>& issues) {
    QStringList lines;
    for (std::size_t i = 0; i < issues.size() && i < 10; ++i) {
        lines << QStringLiteral("・%1 ページ「%2」: %3").arg(issues[i].page).arg(qs(issues[i].text), feature_words(issues[i].features));
    }
    if (issues.size() > 10) lines << QStringLiteral("…ほか %1 件").arg(issues.size() - 10);
    return lines.join(QLatin1Char('\n'));
}

constexpr const char* kRubyNote = "ルビは本文とは別の文字レイヤーです。受取アプリで本文を直しても、ルビは自動では追従しません。";

QImage to_qimage(const render::Image& image) {
    const render::Image rgb = image.convert("RGB");
    const std::string data = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(data.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

// A message box of the app's (its buttons answered by a test through ask::exec).
QAbstractButton* exec_box(QMessageBox& box) {
    ask::exec(&box);
    return box.clickedButton();
}

}  // namespace

QString icc_setting() {
    const QVariant value = settings()->value(QStringLiteral("color/icc"), QString());
    const QString path = value.toString();
    return value.typeId() == QMetaType::QString && !path.isEmpty() && QFileInfo(path).isFile() ? path : QString();
}

void set_icc_setting(const QString& path) { settings()->setValue(QStringLiteral("color/icc"), path); }

ExportDialog::ExportDialog(QWidget* parent, DocPtr episode, std::optional<fs::path> project, std::string actor, bool official_only,
                           std::int64_t current_page)
    : QDialog(parent), episode_(std::move(episode)), project_(std::move(project)), actor_(std::move(actor)), current_page_(current_page) {
    setObjectName(QStringLiteral("export_dialog"));
    setWindowTitle(official_only ? QStringLiteral("正式な書き出し") : QStringLiteral("書き出し"));
    // (Python's 560 at least; less on a screen narrower than that — 1024 × 640 at 200 % is 512 wide: SPEC UX-02)
    const QScreen* screen = QGuiApplication::primaryScreen();
    setMinimumWidth(std::min(560, screen != nullptr ? std::max(200, screen->availableGeometry().width() - 16) : 560));
    which = new QComboBox;
    which->setObjectName(QStringLiteral("which"));
    for (const auto& [label, key] : std::vector<std::pair<QString, QString>>{{QStringLiteral("全部のページ"), QStringLiteral("all")},
                                                                          {QStringLiteral("今のページ（%1）").arg(current_page), QStringLiteral("current")},
                                                                          {QStringLiteral("今の見開き"), QStringLiteral("spread")},
                                                                          {QStringLiteral("範囲を指定"), QStringLiteral("range")}}) {
        which->addItem(label, key);
    }
    range = new QLineEdit;
    range->setObjectName(QStringLiteral("range"));
    range->setPlaceholderText(QStringLiteral("例: 3-5, 8"));
    range->setVisible(false);
    connect(which, &QComboBox::currentIndexChanged, this, [this] {
        range->setVisible(which->currentData().toString() == QLatin1String("range"));
        show_preview();
    });
    connect(range, &QLineEdit::editingFinished, this, &ExportDialog::show_preview);
    preview = new QLabel;
    preview->setObjectName(QStringLiteral("preview"));
    preview->setAlignment(Qt::AlignCenter);
    preview->setMinimumSize(220, 300);
    preview->setStyleSheet(QStringLiteral("background:%1; border-radius: 6px").arg(theme::tokens().surround));
    preview_note = new QLabel;
    theme::role(preview_note, "hint");
    format = new QComboBox;
    format->setObjectName(QStringLiteral("format"));
    for (const formats::Format& fmt : formats::formats()) {
        if (official_only && !fmt.official) continue;
        format->addItem(qs(fmt.label), qs(fmt.key));
    }
    note = new QLabel;
    note->setWordWrap(true);
    theme::role(note, "hint");
    dpi = new QSpinBox;
    dpi->setRange(72, 1200);
    dpi->setSuffix(QStringLiteral(" dpi"));
    width = new QSpinBox;
    width->setRange(200, 4000);
    width->setValue(800);
    width->setSuffix(QStringLiteral(" px"));
    max_height = new QSpinBox;
    max_height->setRange(400, 20000);
    max_height->setValue(1280);
    max_height->setSuffix(QStringLiteral(" px"));
    long_edge = new QSpinBox;
    long_edge->setRange(400, 8000);
    long_edge->setValue(2048);
    long_edge->setSuffix(QStringLiteral(" px"));
    area = new QComboBox;
    for (const std::string_view key : formats::kAreas) area->addItem(area_label(key), qs(key));
    area->setCurrentIndex(area->findData(QStringLiteral("bleed")));
    area->setToolTip(QStringLiteral("印刷所の指定に合わせます。多くは「裁ち落としまで」。トンボ付きは用紙全体"));
    color = new QComboBox;
    for (const auto& [label, key] : std::vector<std::pair<QString, QString>>{
             {QStringLiteral("自動（モノクロの原稿はグレー、カラーは RGB）"), QStringLiteral("auto")},
             {QStringLiteral("RGB（sRGB を埋め込む）"), QStringLiteral("rgb")},
             {QStringLiteral("CMYK"), QStringLiteral("cmyk")},
             {QStringLiteral("グレー"), QStringLiteral("gray")},
             {QStringLiteral("2 階調（白黒の 1 ビット）"), QStringLiteral("bitonal")}}) {
        color->addItem(label, key);
    }
    icc = new QLineEdit(icc_setting());
    icc->setPlaceholderText(QStringLiteral("なし（K 版の黒・総インキ量 320%）"));
    icc->setToolTip(QStringLiteral("印刷所が指定する CMYK のカラープロファイル（Japan Color 2001 Coated など .icc）"));
    auto* pick_icc_button = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick_icc_button, &QPushButton::clicked, this, &ExportDialog::pick_icc);
    icc_row = new QWidget;
    auto* icc_line = new QHBoxLayout(icc_row);
    icc_line->setContentsMargins(0, 0, 0, 0);
    icc_line->addWidget(icc, 1);
    icc_line->addWidget(pick_icc_button);
    screen_on = new QCheckBox(QStringLiteral("グレーを網点にする"));
    screen_on->setToolTip(QStringLiteral("2 階調で書き出すとき、トーン化していないグレーを、しきい値で白か黒にせず網点にします（書き出しでのトーン化）"));
    screen_lpi = new QDoubleSpinBox;
    screen_lpi->setRange(10, 150);
    screen_lpi->setValue(60);
    screen_lpi->setSuffix(QStringLiteral(" 線"));
    screen_shape = new QComboBox;
    for (const auto& [label, key] : screen_shapes()) screen_shape->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    screen_row = new QWidget;
    auto* screen_line = new QHBoxLayout(screen_row);
    screen_line->setContentsMargins(0, 0, 0, 0);
    screen_line->addWidget(screen_on);
    screen_line->addWidget(screen_lpi);
    screen_line->addWidget(screen_shape);
    connect(color, &QComboBox::currentIndexChanged, this, [this] { screen_row->setEnabled(bitonal()); });
    jpeg = new QCheckBox(QStringLiteral("JPEG にする（PNG より軽い）"));
    spreads = new QCheckBox(QStringLiteral("見開きも 1 枚ずつ出す"));
    psd_text = new QComboBox;
    psd_text->setObjectName(QStringLiteral("psd_text"));
    psd_text->addItem(QStringLiteral("見た目優先で画像化（従来どおり）"), QStringLiteral("image"));
    psd_text->addItem(QStringLiteral("文字を編集可能なレイヤーとして保持"), QStringLiteral("editable"));
    psd_text->setToolTip(QStringLiteral("見た目優先: 台詞ごとに、フキダシと文字を 1 枚の画像レイヤーにします（これまでの PSD）。\n"
                                        "編集可能: 台詞ごとのグループに、本文とルビを文字レイヤー、フキダシを画像レイヤーにして入れます"));
    psd_unsupported = new QComboBox;
    psd_unsupported->setObjectName(QStringLiteral("psd_unsupported"));
    psd_unsupported->addItem(QStringLiteral("表せない装飾があれば書き出さない（厳密）"), QStringLiteral("stop"));
    psd_unsupported->addItem(QStringLiteral("表せない装飾のある台詞だけ画像にする（見た目優先）"), QStringLiteral("rasterize"));
    psd_unsupported->setToolTip(QStringLiteral("グラデーションや画像の文字塗り・パスに沿った文字・弓なりの曲げ・四隅のゆがみ・傍点は、"
                                               "PSD の文字レイヤーにできません（対応表: docs/PSD_TEXT_SUPPORT.md）"));
    psd_notes = new QLabel;
    psd_notes->setObjectName(QStringLiteral("psd_notes"));
    psd_notes->setWordWrap(true);
    psd_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    theme::role(psd_notes, "hint");
    auto* psd_box = new QWidget;
    auto* psd_lines = new QVBoxLayout(psd_box);
    psd_lines->setContentsMargins(0, 0, 0, 0);
    psd_lines->addWidget(psd_text);
    psd_lines->addWidget(psd_unsupported);
    psd_lines->addWidget(psd_notes);
    connect(psd_text, &QComboBox::currentIndexChanged, this, [this] { psd_changed(); });
    connect(psd_unsupported, &QComboBox::currentIndexChanged, this, [this] { psd_changed(); });
    official = new QCheckBox(QStringLiteral("正式な書き出し（点検して、書き出しの承認として記録する）"));
    official->setChecked(official_only);
    official->setEnabled(!official_only);
    const QString default_dir = project_ ? QString::fromStdString(core::path_to_utf8(project_->parent_path() / core::path_from_utf8(core::path_to_utf8(project_->stem()) + "_書き出し")))
                                         : QDir::homePath() + QStringLiteral("/genko_書き出し");
    folder = new QLineEdit(QDir::toNativeSeparators(default_dir));
    folder->setObjectName(QStringLiteral("folder"));
    auto* pick = new QPushButton(QStringLiteral("選ぶ…"));
    connect(pick, &QPushButton::clicked, this, &ExportDialog::pick_folder);
    auto* where = new QHBoxLayout;
    where->addWidget(folder, 1);
    where->addWidget(pick);
    form_ = look::form();
    form_->addRow(look::section(QStringLiteral("形式")));
    form_->addRow(QStringLiteral("形式"), format);
    form_->addRow(QString(), note);
    form_->addRow(look::section(QStringLiteral("ページと大きさ")));
    auto* pages_row = new QHBoxLayout;
    pages_row->addWidget(which);
    pages_row->addWidget(range, 1);
    form_->addRow(QStringLiteral("ページ"), pages_row);
    for (const auto& [key, label, widget] : std::vector<std::tuple<std::string, QString, QWidget*>>{
             {"dpi", QStringLiteral("解像度"), dpi}, {"area", QStringLiteral("書き出す範囲"), area}, {"width", QStringLiteral("幅"), width},
             {"max_height", QStringLiteral("1 枚の高さの上限"), max_height}, {"long_edge", QStringLiteral("長辺"), long_edge},
             {"jpeg", QString(), jpeg}, {"spreads", QString(), spreads}, {"psd_text", QStringLiteral("文字"), psd_box}}) {
        form_->addRow(label, widget);
        rows[key] = widget;
    }
    colour_head = look::section(QStringLiteral("色"));
    form_->addRow(colour_head);
    for (const auto& [key, label, widget] : std::vector<std::tuple<std::string, QString, QWidget*>>{
             {"color", QStringLiteral("色"), color}, {"icc", QStringLiteral("カラープロファイル"), icc_row}, {"screen", QStringLiteral("トーン化"), screen_row}}) {
        form_->addRow(label, widget);
        rows[key] = widget;
    }
    form_->addRow(look::section(QStringLiteral("書き出し先")));
    form_->addRow(QStringLiteral("フォルダ"), where);
    form_->addRow(QString(), official);
    look::quiet_labels(form_);
    buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("書き出す"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("やめる"));
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::run);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* side = new QVBoxLayout;
    side->addWidget(preview, 1);
    side->addWidget(preview_note);
    auto* body = new QVBoxLayout;
    body->addLayout(form_);
    body->addStretch(1);
    look::frame(this, look::header(QStringLiteral("書き出し"), QStringLiteral("形式を選ぶと、その形式で決められることだけが並びます。右は書き出される 1 ページ目です。")),
                body, look::card(side, QStringLiteral("書き出される形")), look::footer(buttons));
    look::fit_to_screen(this, QSize(900, 600));
    format->setCurrentIndex(std::max(0, format->findData(QStringLiteral("png"))));  // (PNG unless another is chosen)
    connect(format, &QComboBox::currentIndexChanged, this, [this] { format_changed(); });
    connect(area, &QComboBox::currentIndexChanged, this, [this] { show_preview(); });
    connect(official, &QCheckBox::toggled, this, [this](bool on) {
        which->setEnabled(!on);
        if (on) which->setCurrentIndex(0);
    });
    format_changed();
}

void ExportDialog::format_changed() {
    const formats::Format* fmt = formats::format(format->currentData().toString().toStdString());
    if (fmt == nullptr) return;
    const auto takes = [fmt](std::string_view key) { return std::find(fmt->options.begin(), fmt->options.end(), key) != fmt->options.end(); };
    note->setText(qs(fmt->note));
    for (const auto& [key, widget] : rows) {
        const bool visible = takes(key);
        widget->setVisible(visible);
        if (QWidget* label = form_->labelForField(widget)) label->setVisible(visible);
    }
    colour_head->setVisible(takes("color") || takes("icc") || takes("screen"));  // (no empty heading)
    const int cmyk = color->findData(QStringLiteral("cmyk"));  // (a PNG has no CMYK)
    if (auto* model = qobject_cast<QStandardItemModel*>(color->model())) model->item(cmyk)->setEnabled(fmt->key != "png");
    if (fmt->key == "png" && color->currentData().toString() == QLatin1String("cmyk")) color->setCurrentIndex(color->findData(QStringLiteral("auto")));
    screen_row->setEnabled(bitonal());
    dpi->setValue(static_cast<int>(std::clamp<std::int64_t>(formats::default_dpi(*episode_, fmt->key), INT_MIN, INT_MAX)));
    long_edge->setValue(fmt->key == "kindle" ? 2560 : 2048);
    jpeg->setChecked(fmt->key == "sns");
    if (!official->isEnabled() || !fmt->official) official->setChecked(official->isChecked() && fmt->official);
    official->setVisible(fmt->official);
    show_preview();
}

std::vector<std::int64_t> ExportDialog::pages() const {
    const core::Document& book = *episode_;
    const QString chosen = which->currentData().toString();
    if (chosen == QLatin1String("current")) return {current_page_};
    if (chosen == QLatin1String("spread")) {
        std::set<std::int64_t> both{current_page_};
        for (const auto& page : book.pages) {  // (next(p for p in pages if p.index == current_page): the first)
            if (page->index == core::Num(current_page_)) {
                if (page->spread_with) both.insert(core::py_int(*page->spread_with));
                break;
            }
        }
        return std::vector<std::int64_t>(both.begin(), both.end());
    }
    if (chosen == QLatin1String("range")) return formats::parse_pages(range->text().toStdString(), static_cast<std::int64_t>(book.pages.size()));
    std::vector<std::int64_t> all;
    for (const auto& page : book.pages) all.push_back(core::py_int(page->index));
    return all;
}

void ExportDialog::psd_changed() {
    const bool editable = psd_text->currentData().toString() == QLatin1String("editable");
    psd_unsupported->setVisible(editable);
    psd_notes->setVisible(editable);
    if (!editable) return;
    std::vector<std::int64_t> chosen;
    try {
        chosen = pages();
    } catch (const std::exception&) {  // (the preview says why)
        psd_notes->clear();
        return;
    }
    QStringList notes;
    try {
        const core::Document part = formats::subset(*episode_, chosen);
        const std::vector<formats::PsdTextIssue> issues = formats::psd_text_issues(part);
        if (issues.empty()) {
            notes << QStringLiteral("PSD の文字に表せない装飾のある台詞はありません。");
        } else {
            const bool rasterize = psd_unsupported->currentData().toString() == QLatin1String("rasterize");
            notes << QStringLiteral("PSD の文字に表せない装飾のある台詞: %1 件（%2）")
                         .arg(issues.size())
                         .arg(rasterize ? QStringLiteral("見た目優先: その台詞だけ画像にして書き出します。その台詞の文字は受取アプリで直せません")
                                        : QStringLiteral("厳密: このままでは書き出しを止めます"));
            notes << issue_lines(issues);
        }
        const formats::PsdTextFonts fonts = formats::psd_text_fonts(part);
        if (!fonts.fonts.empty()) {
            QStringList names;
            for (const std::string& name : fonts.fonts) names << qs(name);
            notes << QStringLiteral("使う書体（PSD に埋め込みません。受取側に同じ書体が要ります）: %1").arg(names.join(QStringLiteral("、")));
        }
        for (const Json& warning : fonts.warnings) {
            const QString page = QString::fromStdString(core::py_str(warning.value("page", Json())));
            const std::string id = warning.value("line", std::string());
            QString words;
            for (const auto& p : part.pages) {
                for (const core::StoryLine* line : part.story_for_page(p->index)) {
                    if (line->id == id) words = qs(line->text).remove(QLatin1Char('\n')).left(24);
                }
            }
            const QString detail = qs(warning.value("detail", std::string()));
            notes << (warning.value("kind", std::string()) == "font_missing"
                          ? QStringLiteral("・%1 ページ「%2」: 書体「%3」が見つからないため、既定の書体で描きます").arg(page, words, detail)
                          : QStringLiteral("・%1 ページ「%2」: 「%3」は台詞の書体にないため、別の書体で描きます").arg(page, words, detail));
        }
    } catch (const std::exception& error) {
        notes << wording::error(QString::fromUtf8(error.what()));
    }
    notes << QString::fromUtf8(kRubyNote) + QStringLiteral("（対応表: docs/PSD_TEXT_SUPPORT.md）");
    psd_notes->setText(notes.join(QLatin1Char('\n')));
}

bool ExportDialog::psd_text_go(const std::vector<std::int64_t>& chosen) {
    if (format->currentData().toString() != QLatin1String("psd") || psd_text->currentData().toString() != QLatin1String("editable")) return true;
    std::vector<formats::PsdTextIssue> issues;
    try {
        issues = formats::psd_text_issues(formats::subset(*episode_, chosen));
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    if (issues.empty()) return true;
    if (psd_unsupported->currentData().toString() != QLatin1String("rasterize")) {
        ask::warning(this, QStringLiteral("Genko"),
                     QStringLiteral("PSD の文字に表せない装飾のある台詞が %1 つあるため、編集可能な PSD は書き出しません（厳密）。\n%2\n"
                                    "「表せない装飾のある台詞だけ画像にする（見た目優先）」を選ぶと、その台詞を画像にして書き出せます。")
                         .arg(issues.size())
                         .arg(issue_lines(issues)));
        return false;
    }
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("PSD の文字"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("次の %1 つの台詞は、PSD の文字に表せない装飾があるため、画像にして書き出します（その台詞の文字は受取アプリで直せません）。\n%2")
                    .arg(issues.size())
                    .arg(issue_lines(issues)));
    QPushButton* go = box.addButton(QStringLiteral("画像にして書き出す"), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("やめる"), QMessageBox::RejectRole);
    return exec_box(box) == go;
}

void ExportDialog::show_preview() {
    if (psd_text != nullptr) psd_changed();  // (PSD-01: what the pages chosen have)
    std::vector<std::int64_t> chosen;
    try {
        chosen = pages();
    } catch (const std::exception& error) {
        preview->clear();
        preview_note->setText(QString::fromUtf8(error.what()));
        return;
    }
    const core::Page* page = nullptr;
    for (const auto& p : episode_->pages) {
        if (p->index == core::Num(chosen.front())) {
            page = p.get();
            break;
        }
    }
    if (page == nullptr) return;
    constexpr int kDpi = 30;
    try {
        render::RenderOptions options;
        options.mode = "print";
        options.crop_marks = area->currentData().toString() == QLatin1String("paper");
        options.skip_unported = true;  // (a picture on the screen only)
        render::Image image = render::render_page(*page, kDpi, options, episode_.get()).image;
        const formats::Format* fmt = formats::format(format->currentData().toString().toStdString());
        if (fmt != nullptr && std::find(fmt->options.begin(), fmt->options.end(), "area") != fmt->options.end()) {
            image = formats::crop_to(image, *page, area->currentData().toString().toStdString(), kDpi);
        }
        const int room = std::max(200, preview->height() - 24);
        preview->setPixmap(QPixmap::fromImage(to_qimage(image)).scaledToHeight(room, Qt::SmoothTransformation));
    } catch (const std::exception& error) {
        preview->clear();
        preview_note->setText(wording::error(QString::fromUtf8(error.what())));
        return;
    }
    preview_note->setText(QStringLiteral("%1 ページ（全 %2 ページを書き出す）").arg(chosen.front()).arg(chosen.size()));
}

QString ExportDialog::ask_preflight(const std::vector<Json>& errors) {
    QStringList lines;
    for (std::size_t i = 0; i < errors.size() && i < 10; ++i) {
        const Json& e = errors[i];
        const QString message = QString::fromStdString(core::py_str(e["message"]));
        lines << (core::py_truthy(e["page"]) ? QStringLiteral("・%1 ページ: %2").arg(QString::fromStdString(core::py_str(e["page"])), message)
                                             : QStringLiteral("・%1").arg(message));
    }
    const QString more = errors.size() > 10 ? QStringLiteral("\n…ほか %1 件").arg(errors.size() - 10) : QString();
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("入稿前の点検"));
    box.setIcon(QMessageBox::Warning);
    box.setText(QStringLiteral("書き出す前に点検したところ、止まる問題が %1 件ありました。\n").arg(errors.size()) + lines.join(QLatin1Char('\n')) + more);
    QPushButton* go = box.addButton(QStringLiteral("このまま書き出す"), QMessageBox::AcceptRole);
    QPushButton* fix = box.addButton(QStringLiteral("直す（点検パネルを開く）"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("やめる"), QMessageBox::RejectRole);
    QAbstractButton* clicked = exec_box(box);
    return clicked == go ? QStringLiteral("go") : clicked == fix ? QStringLiteral("fix") : QStringLiteral("stop");
}

void ExportDialog::pick_icc() {
    const QString path = ask::open_path(this, QStringLiteral("CMYK のカラープロファイル"), QStringLiteral("カラープロファイル (*.icc *.icm)"));
    if (path.isEmpty()) return;
    bool ok = false;
    try {
        ok = render::colour::is_cmyk_profile(core::path_from_utf8(path.toStdString()));
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    if (!ok) {
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("CMYK の印刷用プロファイルではありません"));
        return;
    }
    icc->setText(path);
    set_icc_setting(path);
}

void ExportDialog::pick_folder() {
    const QString path = ask::existing_dir(this, QStringLiteral("書き出し先"), folder->text());
    if (!path.isEmpty()) folder->setText(path);
}

formats::RunOptions ExportDialog::options() const {
    formats::RunOptions o;
    o.official = official->isChecked();
    o.actor = actor_;
    o.dpi = dpi->value();
    o.width = width->value();
    o.max_height = max_height->value();
    o.long_edge = long_edge->value();
    o.jpeg = jpeg->isChecked();
    o.spreads = spreads->isChecked();
    o.area = area->currentData().toString().toStdString();
    o.color = color->currentData().toString().toStdString();
    o.icc = icc->text().trimmed().toStdString();
    o.psd_text = psd_text->currentData().toString().toStdString();
    o.psd_rasterize = psd_unsupported->currentData().toString() == QLatin1String("rasterize");
    if (screen_on->isChecked() && bitonal()) {
        o.screen = Json{{"lpi", screen_lpi->value()}, {"shape", screen_shape->currentData().toString().toStdString()}};
    }
    return o;
}

bool ExportDialog::bitonal() const {
    return format->currentData().toString() == QLatin1String("tiff") || color->currentData().toString() == QLatin1String("bitonal");
}

fs::path ExportDialog::out() const {
    QString text = folder->text();
    if (text == QLatin1String("~") || text.startsWith(QStringLiteral("~/")) || text.startsWith(QStringLiteral("~\\"))) text = QDir::homePath() + text.mid(1);
    return core::path_from_utf8(text.toStdString());
}

void ExportDialog::run() {
    if (!episode_->deferred.empty()) {  // (a book whose pages are still being read: the ones not read yet would come out empty)
        ask::warning(this, QStringLiteral("Genko"), wording::error(QStringLiteral("the book's pages are still being read: try again in a moment")));
        return;
    }
    const fs::path dest = out();
    std::vector<std::int64_t> chosen;
    try {
        chosen = pages();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("Genko"), QString::fromUtf8(error.what()));
        return;
    }
    if (!official->isChecked()) {  // (the official export runs its own check and stops by itself)
        Json report;
        try {
            report = formats::checks::book(*episode_, project_);
        } catch (const std::exception& error) {
            ask::warning(this, QStringLiteral("Genko"), QStringLiteral("書き出す前の点検ができませんでした。\n") + wording::error(QString::fromUtf8(error.what())));
            return;
        }
        std::vector<Json> errors;
        for (const Json& issue : report["issues"]) {
            if (issue["level"] != "error") continue;
            const Json& page = issue["page"];
            const bool on_these = !core::py_truthy(page) ||
                                  std::any_of(chosen.begin(), chosen.end(), [&](std::int64_t n) { return page.is_number() && page.get<double>() == static_cast<double>(n); });
            if (on_these) errors.push_back(issue);
        }
        if (!errors.empty()) {
            const QString answer = ask_preflight(errors);
            if (answer == QLatin1String("fix")) {
                fix_requested = true;
                reject();
                return;
            }
            if (answer != QLatin1String("go")) return;
        }
    }
    if (!psd_text_go(chosen)) return;  // (PSD-01: said or asked before anything is written)
    setCursor(Qt::WaitCursor);
    formats::RunOptions o = options();
    o.pages = chosen;
    Json reply;
    try {
        reply = formats::run(*episode_, project_, format->currentData().toString().toStdString(), dest, o);
    } catch (const std::exception& error) {  // (what Python would stop on with a traceback: said, in Japanese)
        reply = Json{{"ok", false}, {"error", error.what()}};
    }
    unsetCursor();
    this->reply = reply;
    if (!reply.value("ok", false)) {
        QStringList reasons;
        if (const auto errs = reply.find("errors"); errs != reply.end() && errs->is_array()) {
            for (std::size_t i = 0; i < errs->size() && i < 12; ++i) reasons << QString::fromStdString(core::py_str(core::get_or((*errs)[i], "message", Json(""))));
        }
        const QString why = reasons.isEmpty() || reasons.join(QLatin1Char('\n')).isEmpty()
                                ? wording::error(QString::fromStdString(core::py_str(core::get_or(reply, "error", Json("")))))
                                : reasons.join(QLatin1Char('\n'));
        ask::warning(this, QStringLiteral("Genko"), QStringLiteral("書き出せませんでした。\n") + why);
        return;
    }
    QString said = QStringLiteral("%1 個のファイルを書き出しました。\n%2").arg(reply["files"].size()).arg(QString::fromStdString(core::path_to_utf8(dest)));
    if (const auto psd = reply.find("psd_text"); psd != reply.end()) {  // (PSD-01: what the editable PSD did)
        said += QStringLiteral("\n文字レイヤー: %1・ルビ: %2・画像にした台詞: %3\n")
                    .arg(QString::fromStdString(core::py_str(psd->value("text_layers", Json(0)))),
                         QString::fromStdString(core::py_str(psd->value("ruby_layers", Json(0)))))
                    .arg(psd->value("rasterized", Json::array()).size()) +
                QString::fromUtf8(kRubyNote);
    }
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Genko"));
    box.setText(said);
    QPushButton* open_button = box.addButton(QStringLiteral("フォルダを開く"), QMessageBox::ActionRole);
    box.addButton(QStringLiteral("閉じる"), QMessageBox::AcceptRole);
    if (exec_box(box) == open_button) QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(core::path_to_utf8(dest))));
    accept();
}

}  // namespace genko::app
