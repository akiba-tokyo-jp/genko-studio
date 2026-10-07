#include "app/material_preview_cache.hpp"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

using genko::app::MaterialPreviewCache;
using genko::app::MaterialPreviewSpec;
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static QByteArray digest(const char* data) { return QCryptographicHash::hash(data, QCryptographicHash::Sha256); }
static QImage picture(const MaterialPreviewSpec& spec, QRgb color = qRgba(8, 16, 32, 128)) {
    QImage result(spec.pixels, QImage::Format_ARGB32);
    result.fill(color);
    return result;
}
static QString path(const QString& root, const QByteArray& data, const MaterialPreviewSpec& spec) {
    return root + QLatin1Char('/') + MaterialPreviewCache::key(data, spec) + QStringLiteral(".png");
}
static void write(const QString& name, const QByteArray& bytes) {
    QFile file(name); check(file.open(QIODevice::WriteOnly), "open fixture");
    check(file.write(bytes) == bytes.size(), "write fixture");
}
static void throws(const std::function<void()>& action) {
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid input must be rejected");
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using Case = std::pair<const char*, std::function<void()>>;
    const std::vector<Case> cases = {
        {"construction-is-lazy", [] {
            QTemporaryDir tmp; check(tmp.isValid(), "temporary root");
            const QString root = tmp.path() + QStringLiteral("/not-created");
            MaterialPreviewCache cache(root);
            check(!QDir(root).exists(), "construction must not create, scan or render cache");
        }},
        {"persistent-reuse", [] {
            QTemporaryDir tmp; check(tmp.isValid(), "temporary root");
            MaterialPreviewSpec spec; const auto data = digest("original material"); int calls = 0;
            auto generate = [&] { ++calls; return picture(spec); };
            const auto first = MaterialPreviewCache(tmp.path()).obtain(data, spec, generate);
            const auto next = MaterialPreviewCache(tmp.path()).obtain(data, spec, generate);
            check(calls == 1, "restart must reuse persisted preview without renderer call");
            check(first == next && next.pixel(0, 0) == qRgba(8, 16, 32, 128), "RGBA pixels must survive cache round trip");
        }},
        {"content-and-display-keys", [] {
            MaterialPreviewSpec spec; const auto data = digest("a"); const auto base = MaterialPreviewCache::key(data, spec);
            check(base.size() == 64, "hex cache key length");
            check(base != MaterialPreviewCache::key(digest("b"), spec), "content digest must invalidate");
            auto next = spec; next.renderer_version += QStringLiteral(".new"); check(base != MaterialPreviewCache::key(data, next), "renderer version");
            next = spec; next.dpi = 150; check(base != MaterialPreviewCache::key(data, next), "DPI");
            next = spec; next.background = qRgba(0, 0, 0, 0); check(base != MaterialPreviewCache::key(data, next), "background");
            next = spec; next.pixels = QSize(72, 72); check(base != MaterialPreviewCache::key(data, next), "pixel dimensions");
            next = spec; next.render_settings = "color-mode=proof"; check(base != MaterialPreviewCache::key(data, next), "render settings");
        }},
        {"corrupt-file-regenerates", [] {
            QTemporaryDir tmp; MaterialPreviewSpec spec; const auto data = digest("a"); int calls = 0;
            auto generate = [&] { ++calls; return picture(spec); };
            MaterialPreviewCache cache(tmp.path()); cache.obtain(data, spec, generate);
            write(path(tmp.path(), data, spec), "broken PNG");
            check(cache.obtain(data, spec, generate) == picture(spec) && calls == 2, "corrupt PNG must regenerate");
        }},
        {"different-valid-png-regenerates", [] {
            QTemporaryDir tmp; MaterialPreviewSpec spec; const auto data = digest("a"); int calls = 0;
            auto generate = [&] { ++calls; return picture(spec); };
            MaterialPreviewCache cache(tmp.path()); cache.obtain(data, spec, generate);
            check(picture(spec, qRgb(240, 0, 0)).save(path(tmp.path(), data, spec), "PNG"), "write wrong PNG");
            check(cache.obtain(data, spec, generate) == picture(spec) && calls == 2, "valid but unbound PNG must regenerate");
        }},
        {"wrong-cached-size-regenerates", [] {
            QTemporaryDir tmp; MaterialPreviewSpec spec; const auto data = digest("a"); int calls = 0;
            auto generate = [&] { ++calls; return picture(spec); };
            MaterialPreviewCache cache(tmp.path()); cache.obtain(data, spec, generate);
            auto wrong = spec; wrong.pixels = QSize(3, 3); auto image = picture(wrong);
            image.setText(QStringLiteral("genko-material-cache-key"), MaterialPreviewCache::key(data, spec));
            check(image.save(path(tmp.path(), data, spec), "PNG"), "write wrong size PNG");
            check(cache.obtain(data, spec, generate) == picture(spec) && calls == 2, "wrong dimensions must regenerate");
        }},
        {"file-count-budget-and-originals", [] {
            QTemporaryDir tmp; MaterialPreviewSpec spec; MaterialPreviewCache cache(tmp.path(), 2);
            write(tmp.path() + QStringLiteral("/original.png"), "preserve original bytes");
            for (const char* id : {"a", "b", "c"}) cache.obtain(digest(id), spec, [&] { return picture(spec); });
            check(QDir(tmp.path()).entryList({QStringLiteral("*.png")}, QDir::Files).size() == 3, "two cache files plus unrelated original");
            QFile original(tmp.path() + QStringLiteral("/original.png")); check(original.open(QIODevice::ReadOnly), "read original");
            check(original.readAll() == "preserve original bytes", "pruning must not remove originals");
        }},
        {"byte-budget-falls-back", [] {
            QTemporaryDir tmp; const auto root = tmp.path() + QStringLiteral("/cache"); MaterialPreviewSpec spec; int calls = 0;
            MaterialPreviewCache cache(root, 100, 1);
            for (int i = 0; i != 2; ++i) check(cache.obtain(digest("a"), spec, [&] { ++calls; return picture(spec); }) == picture(spec), "fallback pixels");
            check(calls == 2 && QDir(root).entryList({QStringLiteral("*.png")}, QDir::Files).isEmpty(), "oversized entries must not persist");
        }},
        {"disabled-and-unwritable-cache", [] {
            QTemporaryDir tmp; MaterialPreviewSpec spec; int calls = 0;
            const auto generate = [&] { ++calls; return picture(spec); };
            MaterialPreviewCache disabled{QString()};
            check(disabled.obtain(digest("a"), spec, generate) == picture(spec), "empty root fallback");
            const auto blocked = tmp.path() + QStringLiteral("/not-a-folder"); write(blocked, "unchanged");
            check(MaterialPreviewCache(blocked).obtain(digest("a"), spec, generate) == picture(spec), "unwritable root fallback");
            check(calls == 2, "disabled cache renderer calls");
        }},
        {"key-shaped-original-is-preserved", [] {
            QTemporaryDir tmp;MaterialPreviewSpec spec;const auto data=digest("collision");
            const QString target=path(tmp.path(),data,spec);const QByteArray original="not disposable original";
            write(target,original);
            check(MaterialPreviewCache(tmp.path()).obtain(data,spec,[&] {return picture(spec);})==picture(spec),"collision must return generated preview");
            QFile f(target);check(f.open(QIODevice::ReadOnly),"read original collision");
            check(f.readAll()==original,"a digest-shaped unowned original must not be overwritten");
        }},
        {"symlink-root-is-preserved", [] {
            QTemporaryDir tmp;MaterialPreviewSpec spec;
            const QString original=tmp.path()+QStringLiteral("/originals");check(QDir().mkdir(original),"mkdir originals");
            const QString linked=tmp.path()+QStringLiteral("/cache");check(QFile::link(original,linked),"link root");
            check(MaterialPreviewCache(linked).obtain(digest("a"),spec,[&] {return picture(spec);})==picture(spec),"symlink fallback");
            check(QDir(original).entryList(QDir::Files).isEmpty(),"symlink root must not receive cache files");
        }},
        {"symlink-entry-is-preserved", [] {
            QTemporaryDir tmp;MaterialPreviewSpec spec;const auto data=digest("linked-entry");
            const QString original=tmp.path()+QStringLiteral("/original.png");const QByteArray bytes="original bytes";write(original,bytes);
            const QString linked=path(tmp.path(),data,spec);check(QFile::link(original,linked),"link entry");
            check(MaterialPreviewCache(tmp.path()).obtain(data,spec,[&] {return picture(spec);})==picture(spec),"entry fallback");
            check(QFileInfo(linked).isSymLink(),"must not replace original symlink");
            QFile f(original);check(f.open(QIODevice::ReadOnly),"read linked original");check(f.readAll()==bytes,"must not write linked original");
        }},
        {"invalid-input-refusal", [] {
            MaterialPreviewSpec spec; const auto data = digest("a");
            throws([&] { MaterialPreviewCache::key(QByteArray("not SHA256"), spec); });
            auto bad = spec; bad.pixels = QSize(0, 56); throws([&] { MaterialPreviewCache::key(data, bad); });
            bad = spec; bad.pixels = QSize(513, 56); throws([&] { MaterialPreviewCache::key(data, bad); });
            bad = spec; bad.dpi = 0; throws([&] { MaterialPreviewCache::key(data, bad); });
            bad = spec; bad.renderer_version.clear(); throws([&] { MaterialPreviewCache::key(data, bad); });
            bad = spec; bad.render_settings = QByteArray((1 << 20) + 1, 'x'); throws([&] { MaterialPreviewCache::key(data, bad); });
        }},
        {"generator-refusal-does-not-persist", [] {
            QTemporaryDir tmp; const auto root = tmp.path() + QStringLiteral("/cache"); MaterialPreviewSpec spec;
            throws([&] { MaterialPreviewCache(root).obtain(digest("a"), spec, [] { return QImage(1, 1, QImage::Format_ARGB32); }); });
            check(!QDir(root).exists(), "invalid generator output must not create cache");
            bool caught = false;
            try { MaterialPreviewCache(root).obtain(digest("a"), spec, []() -> QImage { throw std::runtime_error("renderer failed"); }); }
            catch (const std::runtime_error&) { caught = true; }
            check(caught && !QDir(root).exists(), "renderer error must propagate without cache writes");
        }}
    };
    int count = 0, failures = 0;
    for (const auto& [name, run] : cases) {
        if (argc == 2 && QString::fromLocal8Bit(argv[1]) != QString::fromLatin1(name)) continue;
        ++count;
        try { run(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cout << "FAIL " << name << ": " << e.what() << '\n'; }
    }
    std::cout << "TOTAL " << count << " FAILED " << failures << '\n';
    return count == 0 || failures != 0 ? 1 : 0;
}
