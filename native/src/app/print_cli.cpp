// `genko print` (PC5 I-f): プリント…'s sheets from the command line (app/print_dialog.cpp's print_sheets).

#include "app/print_cli.hpp"

#include <QCryptographicHash>
#include <QGuiApplication>
#include <QImage>
#include <QPageLayout>
#include <QPageSize>
#include <QPrinter>
#include <QPrinterInfo>
#include <QStringList>

#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "app/print_dialog.hpp"
#include "core/error.hpp"
#include "core/json.hpp"
#include "core/paths.hpp"
#include "formats/copy.hpp"
#include "formats/exporting.hpp"
#include "formats/print_marks.hpp"
#include "render/brushes.hpp"
#include "render/not_yet_ported.hpp"
#include "storage/reader.hpp"

namespace genko::app {

using core::Json;
namespace fp = formats::print;

namespace {

void put(const std::string& text, std::FILE* to) {
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

int usage(const std::string& message) {
    put(core::dump_python(Json{{"ok", false}, {"error", "genko print: " + message}, {"code", "usage"}}) + "\n", stderr);
    return 2;
}

int fail(const std::string& message, const std::string& code, bool ascii, const Json& more = Json::object()) {
    Json out{{"ok", false}, {"error", message}, {"code", code}};
    for (const auto& [key, value] : more.items()) out[key] = value;
    put(core::dump_python(out, ascii) + "\n", stdout);
    return 1;
}

std::optional<double> number(const QString& text) {
    bool ok = false;
    const double v = text.trimmed().toDouble(&ok);
    return ok && std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

// The sha256 of a sheet's RGB rows (its picture as render::Image's bytes: no row padding).
std::string rows_sha256(const QImage& picture) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QImage rgb = picture.convertToFormat(QImage::Format_RGB888);
    for (int y = 0; y < rgb.height(); ++y) hash.addData(QByteArrayView(reinterpret_cast<const char*>(rgb.constScanLine(y)), qsizetype(rgb.width()) * 3));
    return hash.result().toHex().toStdString();
}

}  // namespace

int print_command(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // (a Qt without a screen when none is set: PDFs and dry runs where there is no display)
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QStringList args = QCoreApplication::arguments().mid(2);  // (after `genko print`)
    const bool ascii = args.removeAll(QStringLiteral("--ascii")) > 0;
    static const QStringList kValued{QStringLiteral("--pages"),   QStringLiteral("--printer"),  QStringLiteral("--pdf"),         QStringLiteral("--copies"),
                                     QStringLiteral("--area"),    QStringLiteral("--scale"),    QStringLiteral("--position"),    QStringLiteral("--paper"),
                                     QStringLiteral("--orientation"), QStringLiteral("--marks"), QStringLiteral("--profile"),   QStringLiteral("--intent"),
                                     QStringLiteral("--dpi")};
    static const QStringList kFlags{QStringLiteral("--spreads"), QStringLiteral("--no-bpc"), QStringLiteral("--dry-run"), QStringLiteral("--json")};
    std::map<QString, QString> values;
    std::set<QString> flags;
    std::optional<QString> book;
    for (qsizetype i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (kFlags.contains(arg)) {
            flags.insert(arg);
            continue;
        }
        QString key = arg, value;
        bool inline_value = false;
        if (const qsizetype eq = arg.indexOf(QLatin1Char('=')); arg.startsWith(QStringLiteral("--")) && eq > 0) {
            key = arg.left(eq);
            value = arg.mid(eq + 1);
            inline_value = true;
        }
        if (kValued.contains(key)) {
            if (!inline_value) {
                if (i + 1 >= args.size()) return usage("argument " + key.toStdString() + ": expected one argument");
                value = args[++i];
            }
            if (values.contains(key)) return usage("argument " + key.toStdString() + ": given twice");
            values[key] = value;
            continue;
        }
        if (arg.startsWith(QLatin1Char('-')) && arg.size() > 1) return usage("unrecognized arguments: " + arg.toStdString());
        if (book) return usage("unrecognized arguments: " + arg.toStdString());
        book = arg;
    }
    if (!book) return usage("genko print <book> [--pages P] [--printer NAME | --pdf FILE] [--dry-run] …: the following arguments are required: book");
    const auto value = [&](const char* key) -> std::optional<QString> {
        const auto found = values.find(QString::fromLatin1(key));
        return found == values.end() ? std::nullopt : std::optional<QString>(found->second);
    };
    if (value("--printer") && value("--pdf")) return usage("give --printer or --pdf, not both");
    const bool dry = flags.contains(QStringLiteral("--dry-run"));
    // the settings
    fp::Setup setup;
    if (const auto area = value("--area")) {
        setup.area = area->trimmed().toLower().toStdString();
        if (setup.area == "sheet") setup.area = "paper";
        if (setup.area != "trim" && setup.area != "bleed" && setup.area != "paper") return usage("argument --area: trim, bleed or paper");
    }
    if (const auto scale = value("--scale")) {
        const QString s = scale->trimmed().toLower();
        if (s == QLatin1String("fit") || s == QLatin1String("actual")) {
            setup.scale = s.toStdString();
        } else {
            const auto p = number(s.endsWith(QLatin1Char('%')) ? s.chopped(1) : s);
            if (!p || *p < 1.0 || *p > 1000.0) return usage("argument --scale: fit, actual or a percent 1..1000 (e.g. 80%)");
            setup.scale = "percent";
            setup.percent = *p;
        }
    }
    if (const auto position = value("--position")) {
        const QString p = position->trimmed().toLower();
        if (p != QLatin1String("center") && p != QLatin1String("centre")) {
            const QStringList parts = p.split(QLatin1Char(','));
            const char* const wrong = "argument --position: center, or <left>,<top> in mm (within 1000)";
            if (parts.size() != 2) return usage(wrong);
            const std::optional<double> l = number(parts[0]), t = number(parts[1]);
            if (!l.has_value() || !t.has_value()) return usage(wrong);
            const double left = l.value(), top = t.value();
            if (left < -1000.0 || left > 1000.0 || top < -1000.0 || top > 1000.0) return usage(wrong);
            setup.center = false;
            setup.left_mm = left;
            setup.top_mm = top;
        }
    }
    setup.spreads = flags.contains(QStringLiteral("--spreads"));
    if (const auto copies = value("--copies")) {
        bool ok = false;
        setup.copies = copies->trimmed().toInt(&ok);
        if (!ok || setup.copies < 1 || setup.copies > 999) return usage("argument --copies: 1..999");
    }
    if (const auto dpi = value("--dpi")) {
        bool ok = false;
        const int d = dpi->trimmed().toInt(&ok);
        if (!ok || d < 72 || d > 600) return usage("argument --dpi: 72..600");
        setup.dpi = d;
    }
    std::optional<fp::Paper> paper;
    bool landscape = false;
    try {
        if (const auto marks = value("--marks")) setup.marks = fp::marks_of(marks->toStdString());
        if (const auto name = value("--paper")) paper = fp::paper_of(name->trimmed().toStdString());
    } catch (const core::Error& error) {
        return usage(std::string(error.what()));
    }
    if (const auto o = value("--orientation")) {
        const QString t = o->trimmed().toLower();
        if (t != QLatin1String("portrait") && t != QLatin1String("landscape")) return usage("argument --orientation: portrait or landscape");
        landscape = t == QLatin1String("landscape");
    }
    if (const auto profile = value("--profile")) {
        setup.color.genko = true;
        setup.color.profile = profile->toStdString();
    }
    if (const auto intent = value("--intent")) {
        if (!setup.color.genko) return usage("argument --intent: only with --profile");
        setup.color.intent = intent->trimmed().toLower().toStdString();
    }
    if (flags.contains(QStringLiteral("--no-bpc"))) {
        if (!setup.color.genko) return usage("argument --no-bpc: only with --profile");
        setup.color.bpc = false;
    }
    try {
        fp::check(setup);
    } catch (const core::Error& error) {
        return fail(error.what(), error.code(), ascii);
    }
    try {
        const storage::LoadResult loaded = storage::load_document(core::path_from_utf8(book->toStdString()));
        const core::Document& doc = loaded.document;
        // (the review's P2-1: a PDF never into the book or its folder, over a file it links or a picture it was opened from)
        if (const auto pdf = value("--pdf")) {
            try {
                formats::check_target(formats::write_guard(doc, core::path_from_utf8(book->toStdString())), core::path_from_utf8(pdf->toStdString()));
            } catch (const core::Error& error) {
                return usage(std::string("argument --pdf: ") + error.what());
            }
        }
        render::brushes::clear_custom();
        render::brushes::register_book(doc);  // (the book's own brushes, as `genko render` draws its pages)
        const auto count = static_cast<std::int64_t>(doc.pages.size());
        const std::vector<std::int64_t> pages =
            formats::parse_pages(value("--pages") ? value("--pages")->toStdString() : (count > 1 ? "1-" + std::to_string(count) : std::string("1")), count);
        // the printer: a PDF (or a dry run without a printer named) on the paper asked, A4 by default, the whole of it printed
        QPrinter printer(QPrinter::HighResolution);
        const bool to_pdf = value("--pdf").has_value() || (dry && !value("--printer"));
        if (to_pdf) {
            printer.setOutputFormat(QPrinter::PdfFormat);
            if (const auto pdf = value("--pdf"); pdf && !dry) printer.setOutputFileName(*pdf);
            if (!paper) paper = fp::paper_of("a4");
        } else {
            const QStringList known = QPrinterInfo::availablePrinterNames();
            if (const auto name = value("--printer")) {
                if (!known.contains(*name)) return fail("no printer of this name: " + name->toStdString(), "not_found", ascii);
                printer.setPrinterName(*name);
            } else if (QPrinterInfo::defaultPrinter().isNull()) {
                return fail("the computer has no printer (give --pdf FILE to print into a PDF)", "not_found", ascii);
            }
            if (!setup.dpi) setup.dpi = printing::print_dpi(printer);
        }
        if (!setup.dpi) setup.dpi = 300;
        if (paper) {
            // (Qt's own size for a paper it knows — what the printer and the PDF take —, else the size given)
            static const std::map<std::string, QPageSize::PageSizeId> kKnown{
                {"a3", QPageSize::A3},       {"a4", QPageSize::A4},         {"a5", QPageSize::A5},   {"b4", QPageSize::JisB4},
                {"b5", QPageSize::JisB5},    {"letter", QPageSize::Letter}, {"legal", QPageSize::Legal}, {"tabloid", QPageSize::Tabloid}};
            const auto known = kKnown.find(paper->name);
            printer.setPageSize(known != kKnown.end() ? QPageSize(known->second)
                                                      : QPageSize(QSizeF(paper->width_mm, paper->height_mm), QPageSize::Millimeter,
                                                                  QString::fromStdString(paper->name), QPageSize::ExactMatch));
        }
        printer.setPageOrientation(landscape ? QPageLayout::Landscape : QPageLayout::Portrait);
        if (to_pdf) printer.setPageMargins(QMarginsF(0, 0, 0, 0), QPageLayout::Millimeter);
        printer.setCopyCount(setup.copies);
        printer.setDocName(doc.title.empty() ? QStringLiteral("Genko") : QString::fromStdString(doc.title));
        std::vector<std::string> hashes;
        const std::vector<fp::Sheet> sheets = printing::print_sheets(
            doc, printer, pages, setup, {}, [&hashes](const QImage& picture, const QRectF&) { hashes.push_back(rows_sha256(picture)); }, !dry);
        const auto [full, printable] = printing::paper_of(printer);
        const QRectF sheet_mm = printer.pageLayout().fullRect(QPageLayout::Millimeter);
        Json out{{"ok", true}, {"dry_run", dry}};
        if (to_pdf && value("--pdf")) out["pdf"] = value("--pdf")->toStdString();
        if (!to_pdf) out["printer"] = printer.printerName().toStdString();
        out["copies"] = setup.copies;
        out["paper"] = Json{{"name", paper ? paper->name : std::string("printer")},
                            {"width_mm", std::round(sheet_mm.width() * 1000.0) / 1000.0},
                            {"height_mm", std::round(sheet_mm.height() * 1000.0) / 1000.0},
                            {"orientation", landscape ? "landscape" : "portrait"}};
        out["printable_mm"] = Json::array({std::round(printable.x * 1000.0) / 1000.0, std::round(printable.y * 1000.0) / 1000.0,
                                           std::round(printable.w * 1000.0) / 1000.0, std::round(printable.h * 1000.0) / 1000.0});
        Json color{{"handling", setup.color.genko ? "genko" : "printer"}};
        if (setup.color.genko) {
            color["profile"] = setup.color.profile;
            color["intent"] = setup.color.intent;
            color["bpc"] = setup.color.bpc;
        }
        out["color"] = std::move(color);
        Json list = Json::array();
        for (std::size_t i = 0; i < sheets.size(); ++i) {
            Json one = sheets[i].json();
            one["sha256"] = i < hashes.size() ? hashes[i] : std::string();
            list.push_back(std::move(one));
        }
        out["sheets"] = std::move(list);
        put(core::dump_python(out, ascii) + "\n", stdout);
        return 0;
    } catch (const render::NotYetPorted& error) {
        return fail(error.what(), error.code(), ascii, Json{{"element", error.element()}});
    } catch (const core::Error& error) {
        return fail(error.code() == "not_found" ? "not found: " + std::string(error.what()) : std::string(error.what()), error.code(), ascii);
    } catch (const std::exception& error) {
        return fail(error.what(), "internal", ascii);
    }
}

}  // namespace genko::app
