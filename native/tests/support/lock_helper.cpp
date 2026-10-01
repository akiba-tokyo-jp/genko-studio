// genko_lock_helper <dir> [agent]: another process that holds a book's project lock (storage::ProjectLock) until its
// stdin closes. Prints "locked" once it holds it (or "busy: <message>" and exits 3), then "released". Tests only.

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <string>

#include "core/error.hpp"
#include "storage/fsutil.hpp"
#include "storage/lock.hpp"

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: genko_lock_helper <dir> [agent]\n");
        return 2;
    }
    genko::storage::ProjectLock lock(genko::storage::path_from_utf8(args[1].toStdString()),
                                     args.size() > 2 ? args[2].toStdString() : std::string("genko"));
    try {
        lock.try_acquire();
    } catch (const genko::core::Error& error) {
        std::printf("busy: %s\n", error.what());
        std::fflush(stdout);
        return 3;
    }
    std::printf("locked\n");
    std::fflush(stdout);
    char buffer[256];
    while (std::fread(buffer, 1, sizeof buffer, stdin) > 0) {
    }
    lock.release();
    std::printf("released\n");
    std::fflush(stdout);
    return 0;
}
