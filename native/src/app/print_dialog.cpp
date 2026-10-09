// 印刷… (Python's genko/app/printing.py: sheets, print_pages, PrintDialog).

#include "app/print_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPrintDialog>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QPushButton>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>
#include <map>

#include "app/ask.hpp"
#include "app/main_window.hpp"
#include "app/theme.hpp"
#include "app/wording.hpp"
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

// A page as it prints at `dpi`, cut to `area`.
render::Image page_picture(const core::Document& episode, const core::Page& page, const QString& area, int dpi) {
    render::RenderOptions options;
    options.mode = "print";
    options.crop_marks = area == QLatin1String("paper");
    return formats::crop_to(render::render_page(page, dpi, options, &episode).image, page, area.toStdString(), dpi);
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

int print_pages(const core::Document& episode, QPrinter& printer, const std::vector<std::int64_t>& pages, const QString& area,
                const std::function<void(int, int)>& progress, const QString& scale, bool spreads,
                const std::function<void(const QImage&, const QRectF&)>& sheet) {
    const int dpi = print_dpi(printer);
    const auto groups = sheets(episode, pages, spreads);
    // (what this build does not draw yet is said before any paper is used: a quick look at each page first)
    for (const auto& group : groups) {
        for (const core::Page* page : group) {
            render::RenderOptions look;
            look.mode = "print";
            look.skip_unported = true;
            const auto seen = render::render_page(*page, 10, look, &episode);
            if (!seen.omitted.empty()) throw render::NotYetPorted(seen.omitted.front());
        }
    }
    QPainter painter;
    if (!painter.begin(&printer)) throw core::Error("value", "プリンターを開けませんでした");
    int done = 0;
    try {
        for (std::size_t n = 0; n < groups.size(); ++n) {
            if (n > 0) printer.newPage();
            std::vector<render::Image> images;
            for (const core::Page* page : groups[n]) images.push_back(page_picture(episode, *page, area, dpi));
            render::Image joined = images.front();
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
            }
            const QImage picture = qimage(joined);
            const QRectF room(painter.viewport());
            double w = 0.0, h = 0.0;
            if (scale == QLatin1String("actual")) {
                // the page's own size: its pixels at `dpi`, in the printer's device pixels
                const int resolution = printer.resolution();
                const double factor = static_cast<double>(resolution != 0 ? resolution : dpi) / dpi;
                w = picture.width() * factor;
                h = picture.height() * factor;
            } else {
                const double fit = std::min(room.width() / picture.width(), room.height() / picture.height());
                w = picture.width() * fit;
                h = picture.height() * fit;
            }
            const QRectF target(room.x() + (room.width() - w) / 2, room.y() + (room.height() - h) / 2, w, h);
            if (sheet) sheet(picture, target);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(target, picture);
            ++done;
            if (progress) progress(done, static_cast<int>(groups.size()));
        }
    } catch (...) {
        painter.end();
        throw;
    }
    painter.end();
    return done;
}

}  // namespace printing

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
    scale->setToolTip(QStringLiteral("原寸: 原稿の実際の大きさで印刷します（用紙より大きい所は切れます）"));
    spreads = new QCheckBox(QStringLiteral("見開きは 2 ページを 1 枚に"));
    auto* form = new QFormLayout;
    form->addRow(QStringLiteral("ページ"), pages);
    form->addRow(QStringLiteral("範囲"), area);
    form->addRow(QStringLiteral("大きさ"), scale);
    form->addRow(QString(), spreads);
    auto* note = new QLabel(QStringLiteral("印刷と同じ見え方（ネームは出ません）で、用紙に合わせて縮めて印刷します。入稿用のデータは「書き出し…」で作ります。"));
    note->setWordWrap(true);
    theme::role(note, "hint");
    auto* buttons = new QDialogButtonBox;
    print_button = buttons->addButton(QStringLiteral("プリンターを選んで印刷…"), QDialogButtonBox::AcceptRole);
    preview_button = buttons->addButton(QStringLiteral("プレビュー…"), QDialogButtonBox::ActionRole);
    connect(preview_button, &QPushButton::clicked, this, [this] { preview(); });
    this_page = buttons->addButton(QStringLiteral("このページだけ"), QDialogButtonBox::ActionRole);
    connect(this_page, &QPushButton::clicked, this, [this] { pages->setText(current_); });
    cancel_button = buttons->addButton(QStringLiteral("やめる"), QDialogButtonBox::RejectRole);
    connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    connect(print_button, &QPushButton::clicked, this, [this] { print(); });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addWidget(buttons);
}

std::vector<std::int64_t> PrintDialog::chosen() const {
    return formats::parse_pages(pages->text().toStdString(), static_cast<std::int64_t>(window_->book().pages.size()));
}

void PrintDialog::print(QPrinter* given) {
    std::vector<std::int64_t> wanted;
    try {
        wanted = chosen();
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("印刷"), QString::fromUtf8(error.what()).replace(QStringLiteral("書き出す"), QStringLiteral("印刷する")));
        return;
    }
    QPrinter own(QPrinter::HighResolution);
    QPrinter* printer = given;
    if (printer == nullptr) {
        printer = &own;
        const std::string& title = window_->book().title;
        printer->setDocName(title.empty() ? QStringLiteral("Genko") : QString::fromStdString(title));
        QPrintDialog chooser(printer, this);
        chooser.setWindowTitle(QStringLiteral("プリンターを選ぶ"));
        if (ask::exec(&chooser) != QDialog::Accepted) return;
    }
    setCursor(Qt::WaitCursor);
    int printed = 0;
    try {
        printed = printing::print_pages(window_->book(), *printer, wanted, area->currentData().toString(), {}, scale->currentData().toString(),
                                        spreads->isChecked());
    } catch (const core::Error& error) {
        unsetCursor();
        ask::warning(this, QStringLiteral("印刷"), wording::error(QString::fromUtf8(error.what())));
        return;
    }
    unsetCursor();
    window_->statusBar()->showMessage(QStringLiteral("%1 枚を印刷に送りました").arg(printed), 5000);
    accept();
}

QPrintPreviewDialog* PrintDialog::preview() {
    std::vector<std::int64_t> wanted;
    try {
        wanted = chosen();
    } catch (const core::Error& error) {
        ask::warning(this, QStringLiteral("印刷"), QString::fromUtf8(error.what()).replace(QStringLiteral("書き出す"), QStringLiteral("印刷する")));
        return nullptr;
    }
    auto* printer = new QPrinter(QPrinter::HighResolution);
    auto* dialog = new QPrintPreviewDialog(printer, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [printer] { delete printer; });
    dialog->setWindowTitle(QStringLiteral("印刷のプレビュー"));
    const QString chosen_area = area->currentData().toString();
    const QString chosen_scale = scale->currentData().toString();
    const bool both = spreads->isChecked();
    const DocPtr book = window_->session().snapshot();
    connect(dialog, &QPrintPreviewDialog::paintRequested, this, [book, wanted, chosen_area, chosen_scale, both](QPrinter* p) {
        try {
            printing::print_pages(*book, *p, wanted, chosen_area, {}, chosen_scale, both);
        } catch (const core::Error&) {
            // (the preview stays empty: printing says why)
        }
    });
    dialog->open();
    return dialog;
}

}  // namespace genko::app
