#include "app/display_color.hpp"

#include <QFile>
#include <QSettings>
#include <QSysInfo>

#include <lcms2.h>

#include <array>
#include <cmath>
#include <functional>
#include <mutex>
#include <vector>

#include "app/config.hpp"
#include "core/color_model.hpp"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace genko::app::display_color {

namespace {

bool rgb_profile(const core::Bytes& icc) {
    if (!icc || icc->empty()) return false;
    cmsHPROFILE handle = cmsOpenProfileFromMem(icc->data(), static_cast<cmsUInt32Number>(icc->size()));
    if (handle == nullptr) return false;
    const bool rgb = cmsGetColorSpace(handle) == cmsSigRgbData;
    cmsCloseProfile(handle);
    return rgb;
}

core::Bytes read_file(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    const QByteArray bytes = file.read(static_cast<qint64>(core::kProfileMaxBytes) + 1);
    if (bytes.isEmpty() || bytes.size() > static_cast<qsizetype>(core::kProfileMaxBytes)) return nullptr;
    return std::make_shared<const std::string>(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

std::mutex kept_lock;
std::string kept_key;
std::shared_ptr<const Monitor> kept;
bool kept_made = false;

}  // namespace

Monitor::~Monitor() {
    if (transform_ != nullptr) cmsDeleteTransform(static_cast<cmsHTRANSFORM>(transform_));
}

std::shared_ptr<const Monitor> Monitor::to(const core::Bytes& icc) {
    if (!rgb_profile(icc)) return nullptr;
    cmsHPROFILE screen = cmsOpenProfileFromMem(icc->data(), static_cast<cmsUInt32Number>(icc->size()));
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    // (the lattice checked in floats; the pictures go through an 8-bit one, laid out as QImage's RGB32 is in memory)
    cmsHTRANSFORM probe = cmsCreateTransform(srgb, TYPE_RGB_DBL, screen, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                             cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    const bool little = QSysInfo::ByteOrder == QSysInfo::LittleEndian;
    cmsHTRANSFORM eight = cmsCreateTransform(srgb, little ? TYPE_BGRA_8 : TYPE_ARGB_8, screen, little ? TYPE_BGRA_8 : TYPE_ARGB_8,
                                             INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
    bool identity = probe != nullptr;
    for (int b = 0; identity && b < 9; ++b)
        for (int g = 0; identity && g < 9; ++g)
            for (int r = 0; identity && r < 9; ++r) {
                const std::array<double, 3> in{r / 8.0, g / 8.0, b / 8.0};
                std::array<double, 3> out{};
                cmsDoTransform(probe, in.data(), out.data(), 1);
                for (int c = 0; c < 3; ++c)
                    if (std::abs(out[static_cast<std::size_t>(c)] - in[static_cast<std::size_t>(c)]) > 0.5 / 255.0) identity = false;
            }
    if (probe != nullptr) cmsDeleteTransform(probe);
    cmsCloseProfile(srgb);
    cmsCloseProfile(screen);
    if (identity || eight == nullptr) {
        if (eight != nullptr) cmsDeleteTransform(eight);
        return nullptr;
    }
    std::shared_ptr<Monitor> made(new Monitor());
    made->transform_ = eight;
    made->icc_ = icc;
    made->description_ = core::icc_description(*icc);
    return made;
}

void Monitor::apply(QImage& image) const {
    if (image.isNull()) return;
    if (image.format() != QImage::Format_RGB32 && image.format() != QImage::Format_ARGB32) image = image.convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        uchar* row = image.scanLine(y);
        cmsDoTransform(static_cast<cmsHTRANSFORM>(transform_), row, row, static_cast<cmsUInt32Number>(image.width()));
    }
}

core::Bytes system_profile() {
#ifdef Q_OS_WIN
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) return nullptr;
    DWORD size = MAX_PATH;
    std::vector<wchar_t> path(size + 1, L'\0');
    BOOL ok = GetICMProfileW(screen, &size, path.data());
    if (!ok && size > path.size()) {
        path.assign(size + 1, L'\0');
        ok = GetICMProfileW(screen, &size, path.data());
    }
    ReleaseDC(nullptr, screen);
    if (!ok) return nullptr;
    const core::Bytes bytes = read_file(QString::fromWCharArray(path.data()));
    return rgb_profile(bytes) ? bytes : nullptr;
#else
    return nullptr;
#endif
}

core::Bytes monitor_profile(const QString& spec) {
    const QString name = spec.trimmed();
    if (name.isEmpty() || name == QLatin1String("auto")) return system_profile();
    if (const core::BuiltinProfile* builtin = core::builtin_profile(name.toStdString())) {
        return builtin->space == core::ProfileSpace::Rgb ? builtin->bytes : nullptr;  // (srgb: none)
    }
    const core::Bytes bytes = read_file(name);
    return rgb_profile(bytes) ? bytes : nullptr;
}

std::shared_ptr<const Monitor> current() {
    const QString spec = settings()->value(QStringLiteral("color/monitorProfile"), QStringLiteral("auto")).toString();
    const core::Bytes icc = monitor_profile(spec);
    const std::string key = icc ? std::to_string(icc->size()) + ":" + std::to_string(std::hash<std::string>{}(*icc)) : std::string();
    const std::lock_guard<std::mutex> held(kept_lock);
    if (kept_made && key == kept_key) return kept;
    kept = icc ? Monitor::to(icc) : nullptr;
    kept_key = key;
    kept_made = true;
    return kept;
}

}  // namespace genko::app::display_color
