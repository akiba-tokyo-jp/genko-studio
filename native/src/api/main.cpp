#include <string_view>

#include "api/cli.hpp"
#if GENKO_WITH_APP
#include "app/app_main.hpp"
#include "app/links.hpp"
#endif

int main(int argc, char** argv) {
#if GENKO_WITH_APP
    // `genko` alone and `genko app [book]`: the desktop app; every other command is the command line
    if (argc < 2 || std::string_view(argv[1]) == "app") return genko::app::run_app(argc, argv);
    // (Python's `genko register-links`: genko:// links open this app)
    if (std::string_view(argv[1]) == "register-links") return genko::app::links::register_command(argc, argv);
#endif
    return genko::api::run_cli(argc, argv);
}
