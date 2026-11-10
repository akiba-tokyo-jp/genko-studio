#pragma once

// Helpers for the drawing tests (header only; not part of the product).

#include <QDir>
#include <QString>
#include <QStringList>

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "core/json.hpp"
#include "render/image.hpp"
#include "render/png.hpp"
#include "testsupport.hpp"

namespace genko::test {

inline double from_bits(const core::Json& hex) {
    return std::bit_cast<double>(static_cast<std::uint64_t>(std::stoull(hex.get<std::string>(), nullptr, 16)));
}

inline std::string bits_of(double x) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(x)));
    return buf;
}

// A colour as the harness writes it: an int, or a list (a Python tuple).
inline render::Ink ink_of(const core::Json& j) {
    if (j.is_array()) return render::Ink::tuple(j.get<std::vector<std::int64_t>>());
    return render::Ink(static_cast<int>(j.get<std::int64_t>()));
}

// tools/migration/render_harness.py <args>, run by the Python reference.
inline Run render_harness(const QStringList& args, const QString& scratch, int timeout_ms = 1800000) {
    QStringList full{repo_root() + QStringLiteral("/tools/migration/render_harness.py")};
    full += args;
    return run(python_ref(), full, python_env(scratch), timeout_ms);
}

// How two pictures of one mode and size differ: the number of different pixels and the largest difference of a band.
struct PixelDiff {
    long long pixels = 0;
    int largest = 0;
    long long first = -1;  // the first pixel that differs
    double mean = 0.0;     // mean absolute difference per band value, in 0..255
};

inline PixelDiff pixel_diff(const render::Image& a, const render::Image& b) {
    PixelDiff out;
    const std::string x = a.tobytes();
    const std::string y = b.tobytes();
    const int bands = a.bands();
    const std::size_t n = x.size() < y.size() ? x.size() : y.size();
    long long total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int d = std::abs(static_cast<int>(static_cast<unsigned char>(x[i])) - static_cast<int>(static_cast<unsigned char>(y[i])));
        total += d;
        if (d > out.largest) out.largest = d;
        if (d != 0 && out.first < 0) out.first = static_cast<long long>(i) / bands;
    }
    for (std::size_t p = 0; p + static_cast<std::size_t>(bands) <= n; p += static_cast<std::size_t>(bands)) {
        if (x.compare(p, static_cast<std::size_t>(bands), y, p, static_cast<std::size_t>(bands)) != 0) ++out.pixels;
    }
    out.mean = n > 0 ? static_cast<double>(total) / static_cast<double>(n) : 0.0;
    return out;
}

// Where differing pictures are kept for a look (GENKO_TEST_ARTIFACTS, else the system's temporary folder).
inline QString artifact_dir() {
    QString dir = qEnvironmentVariable("GENKO_TEST_ARTIFACTS");
    if (dir.isEmpty()) dir = QDir::tempPath() + QStringLiteral("/genko-render-diffs");
    QDir().mkpath(dir);
    return dir;
}

inline void keep_pictures(const QString& name, const render::Image& cpp, const render::Image& python) {
    const QString base = artifact_dir() + QLatin1Char('/') + name;
    try {
        render::save_png(cpp, (base + QStringLiteral("-cpp.png")).toStdString());
        render::save_png(python, (base + QStringLiteral("-python.png")).toStdString());
    } catch (...) {
    }
}

}  // namespace genko::test
