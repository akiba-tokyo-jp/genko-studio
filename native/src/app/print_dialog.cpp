// 印刷… (Python's genko/app/printing.py: sheets, print_pages, PrintDialog); PC5 I-f: プリント…'s colours, position, marks
// and PDF, 1 部プリント's settings (formats/print_marks).

#include "app/print_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPageLayout>
#include <QPainter>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPrinterInfo>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>

#include "app/ask.hpp"
#include "app/config.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "service/wording.hpp"
#include "core/color_model.hpp"
#include "core/error.hpp"
#include "core/pyconv.hpp"
#include "formats/export.hpp"
#include "formats/exporting.hpp"
#include "render/image.hpp"
#include "render/not_yet_ported.hpp"
#include "render/page.hpp"

namespace genko::app {

namespace printing {

namespace {

namespace fp = formats::print;

// printing.AREAS
const std::vector<std::pair<const char*, const char*>>& areas() {
    static const std::vector<std::pair<const char*, const char*>> list = {
        {"仕上がりで切る", "trim"}, {"裁ち落としまで", "bleed"}, {"用紙全体（トンボ付き）", "paper"}};
    return list;
}

// printing._qimage
QImage qimage(const render::Image& image) {
    const render::Image rgb = image.convert("RGB");
    const std::string data = rgb.tobytes();
    return QImage(reinterpret_cast<const uchar*>(data.data()), rgb.width(), rgb.height(), rgb.width() * 3, QImage::Format_RGB888).copy();
}

}  // namespace

std::vector<std::vector<const core::Page*>> sheets(const core::Document& episode, const std::vector<std::int64_t>& pages, bool spreads) {
    const auto by_index = [&](const core::Num& index) -> const core::Page* {  // ({page.index: page}: the last so numbered)
        const core::Page* found = nullptr;
        for (const auto& page : episode.pages) {
            if (page->index == index) found = page.get();
        }
        return found;
    };
    std::vector<std::vector<const core::Page*>> out;
    std::vector<core::Num> done;
    const auto is_done = [&](const core::Num& index) { return std::find(done.begin(), done.end(), index) != done.end(); };
    for (const std::int64_t index : pages) {
        if (is_done(core::Num(index))) continue;
        const core::Page* page = by_index(core::Num(index));
        if (page == nullptr) throw core::Error("key", std::to_string(index));
        const core::Page* partner = nullptr;
        if (spreads && page->spread_with &&
            std::any_of(pages.begin(), pages.end(), [&](std::int64_t n) { return core::Num(n) == *page->spread_with; })) {
            partner = by_index(*page->spread_with);
        }
        if (partner == nullptr) {
            out.push_back({page});
            done.push_back(core::Num(index));
            continue;
        }
        out.push_back(page->side() == "left" ? std::vector<const core::Page*>{page, partner} : std::vector<const core::Page*>{partner, page});
        done.push_back(page->index);
        done.push_back(partner->index);
    }
    return out;
}

int print_dpi(const QPrinter& printer) {
    const int resolution = printer.resolution();
    return std::max(150, std::min(kMaxDpi, resolution != 0 ? resolution : 300));
}

void check_pages(const core::Document& episode, const std::vector<std::int64_t>& pages, bool spreads) {
    // (a book whose pages are still being read: the ones not read yet would print blank)
    if (!episode.deferred.empty()) throw core::Error("page_not_loaded", "the book's pages are still being read: try again in a moment");
    // (what this build does not draw yet is said before any paper is used: a quick look at each page first)
    for (const auto& group : sheets(episode, pages, spreads)) {
        for (const core::Page* page : group) {
            render::RenderOptions look;
            look.mode = "print";
            look.skip_unported = true;
            const auto seen = render::render_page(*page, 10, look, &episode);
            for (const std::string& element : seen.omitted) {
                if (!element.starts_with("plugin_")) throw render::NotYetPorted(element);
            }
            if (!seen.omitted.empty()) {
                // a filter plugin left out on screen (not chosen, or its runner in trouble): drawn as it prints, so its
                // own refusal is said (one not installed does nothing, as in Python)
                render::RenderOptions output;
                output.mode = "print";
                (void)render::render_page(*page, 10, output, &episode);
            }
        }
    }
}

std::pair<fp::RectMm, fp::RectMm> paper_of(const QPrinter& printer) {
    const int resolution = printer.resolution() != 0 ? printer.resolution() : 300;
    const double mm = 25.4 / resolution;
    const QPageLayout layout = printer.pageLayout();
    const QRect full = layout.fullRectPixels(resolution);
    const fp::RectMm paper{0.0, 0.0, full.width() * mm, full.height() * mm};
    if (printer.fullPage()) return {paper, paper};
    const QRect paint = layout.paintRectPixels(resolution);
    return {paper, fp::RectMm{paint.x() * mm, paint.y() * mm, paint.width() * mm, paint.height() * mm}};
}

std::vector<fp::Sheet> print_sheets(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages, const fp::Setup& setup,
                                    const std::function<void(int, int)>& progress, const std::function<void(const QImage&, const QRectF&)>& sheet,
                                    bool draw) {
    fp::check(setup);
    const int dpi = setup.dpi.value_or(print_dpi(printer));
    check_pages(episode, pages, setup.spreads);
    const auto groups = sheets(episode, pages, setup.spreads);
    const auto [paper, printable] = paper_of(printer);
    const int resolution = printer.resolution() != 0 ? printer.resolution() : dpi;
    const double k = resolution / 25.4;  // (device pixels a mm)
    const std::string description = setup.marks.description ? fp::description_of(episode) : std::string();
    QPainter painter;
    if (draw && !painter.begin(&printer)) throw core::Error("value", "プリンターを開けませんでした");
    std::vector<fp::Sheet> out;
    try {
        for (std::size_t n = 0; n < groups.size(); ++n) {
            if (draw && n > 0) printer.newPage();
            std::vector<render::Image> images;
            for (const core::Page* page : groups[n]) images.push_back(fp::page_picture(episode, *page, setup.area, dpi, setup.color));
            render::Image joined = images.front();
            fp::RectMm trim = fp::trim_in(*groups[n].front(), setup.area, dpi);
            if (images.size() > 1) {
                int w = 0, h = 0;
                for (const render::Image& image : images) {
                    w += image.width();
                    h = std::max(h, image.height());
                }
                joined = render::Image::create("RGB", render::Size{w, h}, render::Ink{255, 255, 255});
                int x = 0;
                for (const render::Image& image : images) {
                    joined.paste(image.convert("RGB"), render::Point{x, 0});
                    x += image.width();
                }
                // (the spread's finished size: from the left page's to the right page's)
                const fp::RectMm right = fp::trim_in(*groups[n].back(), setup.area, dpi);
                const double x1 = images.front().width() + right.x + right.w;
                trim.h = std::max(trim.y + trim.h, right.y + right.h) - std::min(trim.y, right.y);
                trim.y = std::min(trim.y, right.y);
                trim.w = x1 - trim.x;
            }
            fp::Sheet placed = fp::place(joined.width(), joined.height(), dpi, trim, paper, printable, setup,
                                         setup.marks.labels ? fp::label_of(episode, groups[n]) : std::string(), description);
            for (const core::Page* page : groups[n]) placed.pages.push_back(static_cast<std::int64_t>(std::llround(page->index.value())));
            const QImage picture = qimage(joined);
            const auto at = [&](double x, double y) { return QPointF((x - printable.x) * k, (y - printable.y) * k); };
            const QRectF target(at(placed.image.x, placed.image.y), QSizeF(placed.image.w * k, placed.image.h * k));
            if (sheet) sheet(picture, target);
            if (draw) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.drawImage(target, picture);
                if (!placed.lines.empty() || !placed.texts.empty()) {
                    painter.save();
                    painter.setRenderHint(QPainter::Antialiasing);
                    QPen pen(Qt::black);
                    pen.setWidthF(fp::kMarkWidthMm * k);
                    painter.setPen(pen);
                    painter.setBrush(Qt::NoBrush);
                    for (const fp::Line& l : placed.lines) painter.drawLine(at(l.x0, l.y0), at(l.x1, l.y1));
                    for (const fp::Circle& c : placed.circles) painter.drawEllipse(at(c.x, c.y), c.r * k, c.r * k);
                    QFont font = painter.font();
                    font.setPointSizeF(fp::kTextPt);
                    painter.setFont(font);
                    for (const fp::Text& t : placed.texts) painter.drawText(at(t.x, t.y), QString::fromStdString(t.text));
                    painter.restore();
                }
            }
            out.push_back(std::move(placed));
            if (progress) progress(static_cast<int>(out.size()), static_cast<int>(groups.size()));
        }
    } catch (...) {
        if (draw) painter.end();
        throw;
    }
    if (draw) painter.end();
    return out;
}

int print_pages(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages, const QString& area,
                const std::function<void(int, int)>& progress, const QString& scale, bool spreads,
                const std::function<void(const QImage&, const QRectF&)>& sheet) {
    fp::Setup setup;
    setup.area = area.toStdString();
    setup.scale = scale.toStdString();
    setup.spreads = spreads;
    return static_cast<int>(print_sheets(episode, printer, pages, setup, progress, sheet).size());
}

fp::Setup last_setup() {
    const QString kept = settings()->value(QStringLiteral("print/last")).toString();
    if (kept.isEmpty()) return {};
    const core::Json value = core::Json::parse(kept.toStdString(), nullptr, false);
    return value.is_discarded() ? fp::Setup{} : fp::Setup::from_json(value);
}

QString last_printer() { return settings()->value(QStringLiteral("print/printer")).toString(); }

void remember(const fp::Setup& setup, const QString& printer) {
    auto store = settings();
    store->setValue(QStringLiteral("print/last"), QString::fromStdString(setup.json().dump()));
    if (!printer.isEmpty()) store->setValue(QStringLiteral("print/printer"), printer);
}

}  // namespace printing

namespace {

namespace fp = formats::print;

QDoubleSpinBox* mm_box() {
    auto* box = new QDoubleSpinBox;
    box->setRange(-1000.0, 1000.0);
    box->setDecimals(1);
    box->setSuffix(QStringLiteral(" mm"));
    return box;
}

}  // namespace

PrintDialog::PrintDialog(MainWindow* window) : QDialog(window), window_(window) {
    setObjectName(QStringLiteral("print_dialog"));
    setWindowTitle(QStringLiteral("印刷"));
    const auto count = static_cast<std::int64_t>(window->book().pages.size());
    pages = new QLineEdit(count > 1 ? QStringLiteral("1-%1").arg(count) : QStringLiteral("1"));
    pages->setObjectName(QStringLiteral("pages"));
    pages->setToolTip(QStringLiteral("例: 1-4, 7（全ページなら 1-%1）").arg(count));
    current_ = window->current_page() != nullptr ? QString::fromStdString(window->current_page()->index.repr()) : QStringLiteral("1");
    area = new QComboBox;
    for (const auto& [label, key] : printing::areas()) area->addItem(QString::fromUtf8(label), QString::fromLatin1(key));
    scale = new QComboBox;
    scale->addItem(QStringLiteral("用紙に合わせる"), QStringLiteral("fit"));
    scale->addItem(QStringLiteral("原寸（100%）"), QStringLiteral("actual"));
    scale->addItem(QStringLiteral("拡大・縮小"), QStringLiteral("percent"));  // (PC5 I-f: Photoshop's 拡大・縮小 %)
    scale->setToolTip(QStringLiteral("原寸: 原稿の実際の大きさで印刷します（用紙より大きい所は切れます）"));
    spreads = new QCheckBox(QStringLiteral("見開きは 2 ページを 1 枚に"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("ページ"), pages);
    form->addRow(QStringLiteral("範囲"), area);
    form->addRow(QStringLiteral("大きさ"), scale);
    form->addRow(QString(), spreads);
    auto* first = new QWidget;
    first->setLayout(form);

    // 位置と大きさ (Photoshop's 位置とサイズ)
    percent = new QDoubleSpinBox;
    percent->setObjectName(QStringLiteral("percent"));
    percent->setRange(1.0, 1000.0);
    percent->setDecimals(1);
    percent->setValue(100.0);
    percent->setSuffix(QStringLiteral(" %"));
    center = new QCheckBox(QStringLiteral("中央に配置"));
    center->setChecked(true);
    left = mm_box();
    top = mm_box();
    left->setToolTip(QStringLiteral("用紙の左端から"));
    top->setToolTip(QStringLiteral("用紙の上端から"));
    auto* position = new QFormLayout;
    position->addRow(QStringLiteral("拡大・縮小"), percent);
    position->addRow(QString(), center);
    position->addRow(QStringLiteral("上"), top);
    position->addRow(QStringLiteral("左"), left);
    auto* second = new QWidget;
    second->setLayout(position);

    // 印 (Photoshop's トリムマーク)
    corner_marks = new QCheckBox(QStringLiteral("コーナートンボ"));
    center_marks = new QCheckBox(QStringLiteral("センタートンボ"));
    registration_marks = new QCheckBox(QStringLiteral("レジストレーションマーク"));
    description = new QCheckBox(QStringLiteral("説明"));
    description->setToolTip(QStringLiteral("ファイル情報… の説明を、絵の下に入れます"));
    labels = new QCheckBox(QStringLiteral("ラベル"));
    labels->setToolTip(QStringLiteral("原稿の題名とページ番号を、絵の上に入れます"));
    auto* marks = new QVBoxLayout;
    for (QCheckBox* box : {corner_marks, center_marks, registration_marks, description, labels}) marks->addWidget(box);
    marks->addStretch(1);
    auto* third = new QWidget;
    third->setLayout(marks);

    // カラーマネジメント
    color_handling = new QComboBox;
    color_handling->setObjectName(QStringLiteral("color_handling"));
    color_handling->addItem(QStringLiteral("プリンターによるカラー管理"), QStringLiteral("printer"));
    color_handling->addItem(QStringLiteral("Genko によるカラー管理"), QStringLiteral("genko"));
    printer_profile = new QComboBox;
    printer_profile->setObjectName(QStringLiteral("printer_profile"));
    for (const core::BuiltinProfile& profile : core::builtin_profiles()) {
        if (profile.space != core::ProfileSpace::Rgb) continue;  // (Qt hands the printer RGB)
        printer_profile->addItem(QString::fromUtf8(profile.description.data(), static_cast<qsizetype>(profile.description.size())),
                                 QString::fromUtf8(profile.id.data(), static_cast<qsizetype>(profile.id.size())));
    }
    profile_file = new QPushButton(QStringLiteral("ファイル…"));
    connect(profile_file, &QPushButton::clicked, this, [this] {
        const QString file = ask::open_path(this, QStringLiteral("プリンタープロファイル"), QStringLiteral("ICC プロファイル (*.icc *.icm)"));
        if (file.isEmpty()) return;
        printer_profile->addItem(QFileInfo(file).fileName(), file);
        printer_profile->setCurrentIndex(printer_profile->count() - 1);
    });
    intent = new QComboBox;
    intent->addItem(QStringLiteral("知覚的"), QStringLiteral("perceptual"));
    intent->addItem(QStringLiteral("彩度"), QStringLiteral("saturation"));
    intent->addItem(QStringLiteral("相対的な色域を維持"), QStringLiteral("relative"));
    intent->addItem(QStringLiteral("絶対的な色域を維持"), QStringLiteral("absolute"));
    intent->setCurrentIndex(2);
    bpc = new QCheckBox(QStringLiteral("黒点の補正"));
    bpc->setChecked(true);
    color_note = new QLabel(QStringLiteral("プリンターには RGB の絵を渡すため、プリンタープロファイルは RGB のものだけを選べます"
                                           "（CMYK・グレーのプロファイルは選べません）。"));
    color_note->setWordWrap(true);
    theme::role(color_note, "hint");
    auto* chosen_profile = new QHBoxLayout;
    chosen_profile->addWidget(printer_profile, 1);
    chosen_profile->addWidget(profile_file);
    auto* colors = new QFormLayout;
    colors->addRow(QStringLiteral("カラー処理"), color_handling);
    colors->addRow(QStringLiteral("プリンタープロファイル"), chosen_profile);
    colors->addRow(QStringLiteral("マッチング方法"), intent);
    colors->addRow(QString(), bpc);
    colors->addRow(color_note);
    auto* fourth = new QWidget;
    fourth->setLayout(colors);

    tabs = new QTabWidget;
    tabs->addTab(first, QStringLiteral("ページ"));
    tabs->addTab(second, QStringLiteral("位置と大きさ"));
    tabs->addTab(third, QStringLiteral("トンボと印"));
    tabs->addTab(fourth, QStringLiteral("カラーマネジメント"));

    auto* note = new QLabel(QStringLiteral("印刷と同じ見え方（ネームは出ません）で、用紙に合わせて縮めて印刷します。入稿用のデータは「原稿を書き出す…」で作ります。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    auto* buttons = new QDialogButtonBox;
    print_button = buttons->addButton(QStringLiteral("プリンターを選んで印刷…"), QDialogButtonBox::AcceptRole);
    pdf_button = buttons->addButton(QStringLiteral("PDF に出力…"), QDialogButtonBox::ActionRole);
    connect(pdf_button, &QPushButton::clicked, this, [this] { print_pdf(); });
    preview_button = buttons->addButton(QStringLiteral("プレビュー…"), QDialogButtonBox::ActionRole);
    connect(preview_button, &QPushButton::clicked, this, [this] { preview(); });
    this_page = buttons->addButton(QStringLiteral("このページだけ"), QDialogButtonBox::ActionRole);
    connect(this_page, &QPushButton::clicked, this, [this] { pages->setText(current_); });
    cancel_button = buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(print_button, &QPushButton::clicked, this, [this] { print(); });
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(note);
    layout->addWidget(buttons);
    for (QComboBox* box : {area, scale, color_handling}) connect(box, &QComboBox::currentIndexChanged, this, [this] { follow(); });
    connect(center, &QCheckBox::toggled, this, [this] { follow(); });
    // (as the last print left them — 1 部プリント prints so —; the pages always all)
    show_setup(printing::last_setup());
}

void PrintDialog::follow() {
    percent->setEnabled(scale->currentData().toString() == QLatin1String("percent"));
    left->setEnabled(!center->isChecked());
    top->setEnabled(!center->isChecked());
    const bool genko = color_handling->currentData().toString() == QLatin1String("genko");
    for (QWidget* field : std::vector<QWidget*>{printer_profile, profile_file, intent, bpc}) field->setEnabled(genko);
    const bool whole = area->currentData().toString() == QLatin1String("paper");
    corner_marks->setEnabled(!whole);
    corner_marks->setToolTip(whole ? QStringLiteral("用紙全体には、原稿のトンボが入っています") : QString());
}

fp::Setup PrintDialog::setup() const {
    fp::Setup out;
    out.area = area->currentData().toString().toStdString();
    out.scale = scale->currentData().toString().toStdString();
    out.percent = percent->value();
    out.center = center->isChecked();
    out.left_mm = left->value();
    out.top_mm = top->value();
    out.spreads = spreads->isChecked();
    out.marks.corner = corner_marks->isChecked() && out.area != "paper";
    out.marks.center = center_marks->isChecked();
    out.marks.registration = registration_marks->isChecked();
    out.marks.description = description->isChecked();
    out.marks.labels = labels->isChecked();
    out.color.genko = color_handling->currentData().toString() == QLatin1String("genko");
    out.color.profile = printer_profile->currentData().toString().toStdString();
    out.color.intent = intent->currentData().toString().toStdString();
    out.color.bpc = bpc->isChecked();
    return out;
}

void PrintDialog::show_setup(const fp::Setup& s) {
    const auto choose = [](QComboBox* box, const std::string& key) {
        const int at = box->findData(QString::fromStdString(key));
        if (at >= 0) box->setCurrentIndex(at);
    };
    choose(area, s.area);
    choose(scale, s.scale);
    percent->setValue(s.percent);
    center->setChecked(s.center);
    left->setValue(s.left_mm);
    top->setValue(s.top_mm);
    spreads->setChecked(s.spreads);
    corner_marks->setChecked(s.marks.corner);
    center_marks->setChecked(s.marks.center);
    registration_marks->setChecked(s.marks.registration);
    description->setChecked(s.marks.description);
    labels->setChecked(s.marks.labels);
    color_handling->setCurrentIndex(s.color.genko ? 1 : 0);
    if (!s.color.profile.empty()) {
        const QString profile = QString::fromStdString(s.color.profile);
        if (printer_profile->findData(profile) < 0) printer_profile->addItem(QFileInfo(profile).fileName(), profile);
        printer_profile->setCurrentIndex(printer_profile->findData(profile));
    }
    choose(intent, s.color.intent);
    bpc->setChecked(s.color.bpc);
    follow();
}

std::vector<std::int64_t> PrintDialog::chosen() const {
    return formats::parse_pages(pages->text().toStdString(), static_cast<std::int64_t>(window_->book().pages.size()));
}

bool PrintDialog::usable(const fp::Setup& s, std::vector<std::int64_t>& wanted) {
    try {
        wanted = chosen();
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("印刷"), QString::fromUtf8(error.what()).replace(QStringLiteral("書き出す"), QStringLiteral("印刷する")));
        return false;
    }
    try {
        fp::check(s);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("印刷"), wording::error(QString::fromUtf8(error.what())));
        return false;
    }
    return true;
}

void PrintDialog::print(QPrinter* given) {
    const fp::Setup chosen_setup = setup();
    std::vector<std::int64_t> wanted;
    if (!usable(chosen_setup, wanted)) return;
    QPrinter own(QPrinter::HighResolution);
    QPrinter* printer = given;
    if (printer == nullptr) {
        printer = &own;
        const std::string& title = window_->book().title;
        printer->setDocName(title.empty() ? QStringLiteral("Genko") : QString::fromStdString(title));
        // (the printer the last print went to, while the computer has it)
        if (const QString last = printing::last_printer(); !last.isEmpty() && QPrinterInfo::availablePrinterNames().contains(last)) printer->setPrinterName(last);
        QPrintDialog chooser(printer, this);
        chooser.setWindowTitle(QStringLiteral("プリンターを選ぶ"));
        if (ask::exec(&chooser) != QDialog::Accepted) return;
    }
    setCursor(Qt::WaitCursor);
    int printed = 0;
    try {
        printed = static_cast<int>(printing::print_sheets(window_->book(), *printer, wanted, chosen_setup).size());
    } catch (const std::exception& error) {
        unsetCursor();
        ask::warning(this, QStringLiteral("印刷"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    unsetCursor();
    printing::remember(chosen_setup, printer->outputFormat() == QPrinter::NativeFormat ? printer->printerName() : QString());
    window_->statusBar()->showMessage(QStringLiteral("%1 枚を印刷に送りました").arg(printed), 5000);
    accept();
}

void PrintDialog::print_pdf(const QString& file) {
    const fp::Setup chosen_setup = setup();
    std::vector<std::int64_t> wanted;
    if (!usable(chosen_setup, wanted)) return;
    const std::string& title = window_->book().title;
    const QString name = title.empty() ? QStringLiteral("無題") : QString::fromStdString(title);
    const QString path = !file.isEmpty() ? file : ask::save_path(this, QStringLiteral("PDF に出力"), name + QStringLiteral(".pdf"), QStringLiteral("PDF (*.pdf)"));
    if (path.isEmpty()) return;
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(path);
    printer.setDocName(name);
    setCursor(Qt::WaitCursor);
    int printed = 0;
    try {
        printed = static_cast<int>(printing::print_sheets(window_->book(), printer, wanted, chosen_setup).size());
    } catch (const std::exception& error) {
        unsetCursor();
        ask::warning(this, QStringLiteral("印刷"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    unsetCursor();
    printing::remember(chosen_setup, QString());
    window_->statusBar()->showMessage(QStringLiteral("%1 枚を PDF に書きました: %2").arg(printed).arg(path), 5000);
    accept();
}

QPrintPreviewDialog* PrintDialog::preview() {
    const fp::Setup chosen_setup = setup();
    std::vector<std::int64_t> wanted;
    if (!usable(chosen_setup, wanted)) return nullptr;
    const DocPtr book = window_->session().snapshot();
    try {  // (what stops the print is said before the preview opens: not an empty preview)
        printing::check_pages(*book, wanted, chosen_setup.spreads);
    } catch (const std::exception& error) {
        ask::warning(this, QStringLiteral("印刷"), wording::error(QString::fromUtf8(error.what())));
        return nullptr;
    }
    auto* printer = new QPrinter(QPrinter::HighResolution);
    auto* dialog = new QPrintPreviewDialog(printer, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [printer] { delete printer; });
    dialog->setWindowTitle(QStringLiteral("印刷のプレビュー"));
    connect(dialog, &QPrintPreviewDialog::paintRequested, this, [this, book, wanted, chosen_setup](QPrinter* p) {
        try {
            printing::print_sheets(*book, *p, wanted, chosen_setup);
        } catch (const std::exception& error) {
            // (the sheets could not be drawn — the preview stays empty, its 印刷 prints nothing: said once it has painted)
            const QString why = wording::error(QString::fromUtf8(error.what()));
            QMetaObject::invokeMethod(this, [this, why] { ask::warning(this, QStringLiteral("印刷"), why); }, Qt::QueuedConnection);
        }
    });
    dialog->open();
    return dialog;
}

}  // namespace genko::app
