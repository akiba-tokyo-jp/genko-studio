#pragma once

#include <QImage>
#include <QString>

#include <memory>
#include <string>

#include "core/ink.hpp"

// カラー設定 → モニターのプロファイル (SPEC COLOR-03, PC1b; PhotoCraft v0.2.0's engine/src/display_color.rs, MIT): the
// page on screen in the monitor's colours. The tiles hold the page in sRGB; this takes them to the monitor's profile
// (relative colorimetric, black point compensated, as Photoshop shows a document), an 8-bit transform made once for a
// profile. When the monitor is sRGB (the common case, and every system without a profile) nothing is done. Display
// only: never the book, the eyedropper's colour or what is exported.

namespace genko::app::display_color {

class Monitor {
public:
    // sRGB → the profile (RGB ICC bytes; null: sRGB). Null when that is the identity (every point of a 9³ lattice within
    // half an 8-bit step, as upstream's is_identity) or the profile cannot be used.
    static std::shared_ptr<const Monitor> to(const core::Bytes& icc);
    ~Monitor();
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

    // The picture (Format_RGB32 or ARGB32) in the monitor's colours, in place.
    void apply(QImage& image) const;
    // The profile's own description.
    const std::string& description() const { return description_; }
    const core::Bytes& icc() const { return icc_; }

private:
    Monitor() = default;
    void* transform_ = nullptr;
    core::Bytes icc_;
    std::string description_;
};

// The profile カラー設定 names (color/monitorProfile): "auto" (the system's profile of the main display, where the system
// gives one: Windows), a built-in RGB profile's id, or an ICC file. Its bytes; null: sRGB (also for one that cannot
// be read or is not RGB).
core::Bytes monitor_profile(const QString& spec);
// The system's profile of the main display (Windows: GetICMProfileW for the screen; elsewhere none: sRGB).
core::Bytes system_profile();
// The transform for the profile カラー設定 names now (null: none to do).
std::shared_ptr<const Monitor> current();

}  // namespace genko::app::display_color
