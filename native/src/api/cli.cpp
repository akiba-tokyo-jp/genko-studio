#include "api/cli.hpp"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>
#include <string>

#include "api/buildinfo.hpp"

namespace genko::api {

namespace {

void write_json(const nlohmann::json& value, std::FILE* to = stdout) {
    const std::string text = value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    std::fwrite(text.data(), 1, text.size(), to);
    std::fflush(to);
}

int usage(const std::string& message) {
    write_json({{"ok", false}, {"error", message}, {"code", "usage"}}, stderr);
    return 2;
}

}  // namespace

int run_cli(int argc, char** argv) {
    QCoreApplication app(argc, argv);  // (arguments in UTF-8 on Windows too)
    const QStringList args = QCoreApplication::arguments().mid(1);
    if (args.isEmpty()) return usage("genko <command> — try `genko --version`");
    const QString command = args.first();
    if (command == "--version" || command == "version") {
        write_json({{"ok", true}, {"build", build_info()}});
        return 0;
    }
    return usage("unknown command: " + command.toStdString());
}

}  // namespace genko::api
