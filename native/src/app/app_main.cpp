#include "app/app_main.hpp"

#include <QApplication>
#include <QDialog>
#include <QFont>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "app/config.hpp"
#include "app/dialogs.hpp"
#include "app/documents.hpp"
#include "app/icons.hpp"
#include "app/main_window.hpp"
#include "app/perf.hpp"
#include "app/test_script.hpp"
#include "app/theme.hpp"
#include "core/paths.hpp"

namespace genko::app {

namespace fs = std::filesystem;

int run_app(int argc, char** argv) {
    // Validate control arguments before Qt consumes its own switches or initializes the display.
    // Only the ASCII controls are read from CRT argv; the accepted path is decoded by Qt below.
    bool has_path = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            std::fputs("genko app [book]: the desktop app (with no book, the start screen)\n", stdout);
            return 0;
        }
        if (arg.starts_with("-") || has_path) {
            std::fprintf(stderr, "genko app: unrecognized arguments: %s\n", argv[i]);
            return 2;
        }
        has_path = true;
    }
    if (qEnvironmentVariableIsSet("QT_LOGGING_RULES") && !qEnvironmentVariableIsSet("QT_FORCE_STDERR_LOGGING")) {
        // (asked for Qt's log: on Windows it goes to the debugger unless told otherwise, so a log file stays empty)
        qputenv("QT_FORCE_STDERR_LOGGING", "1");
    }
    // Qt reads the native wide command line on Windows; CRT argv loses Japanese paths.
    QApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments().mid(2);
    std::optional<fs::path> path;
    if (!arguments.isEmpty()) path = core::path_from_utf8(arguments.front().toStdString());
    QApplication::setApplicationName(QStringLiteral("Genko Studio"));
    qRegisterMetaType<BookChange>();
    qRegisterMetaType<StrokeInput>();
    icons::init_resources();
    perf::event("app_start");
    if (const int pt = settings()->value(QStringLiteral("ui/font_pt"), 0).toInt(); pt > 0) {  // the letters' size chosen in 環境設定
        QFont font = QApplication::font();
        font.setPointSize(pt);
        QApplication::setFont(font);
    }
    theme::apply(&app);  // (the look before the first window: the start screen too)
    test_script::start();
    if (!path) {
        StartDialog start;
        if (start.exec() != QDialog::Accepted || !start.chosen) return 0;
        path = start.chosen;
    }
    // The window comes at once; the book is read on a worker thread and takes the untitled book's place.
    auto* window = new MainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    window->open_project(*path);
    const int code = QApplication::exec();
    // (windows still open when the loop was ended some other way: closed without asking, their books as they are)
    for (MainWindow* left : documents::windows()) delete left;
    perf::flush();
    return code;
}

}  // namespace genko::app
