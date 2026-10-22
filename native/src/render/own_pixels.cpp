#include "render/own_pixels.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "render/image.hpp"
#include "render/raster.hpp"

namespace genko::render::own_pixels {

Window clamp_window(Window w, int width, int height) {
    if (width <= 0 || height <= 0) return w;  // (no grid to keep it on: a patch's picture lies anywhere)
    w.x0 = std::clamp(w.x0, 0, width);
    w.x1 = std::clamp(w.x1, 0, width);
    w.y0 = std::clamp(w.y0, 0, height);
    w.y1 = std::clamp(w.y1, 0, height);
    return w;
}

Window window_of(double x0, double y0, double x1, double y1, int pad) {
    const double big = 1e8;
    return Window{static_cast<int>(std::clamp(std::floor(x0), -big, big)) - pad, static_cast<int>(std::clamp(std::floor(y0), -big, big)) - pad,
                  static_cast<int>(std::clamp(std::ceil(x1), -big, big)) + pad, static_cast<int>(std::clamp(std::ceil(y1), -big, big)) + pad};
}

std::optional<OwnPixels> OwnPixels::of(const core::Page& page, const core::Layer& layer) {
    OwnPixels p;
    if (layer.color_raster) {
        p.kind_ = Kind::Precise;
        p.precise_ = std::make_shared<core::ColorRasterEdit>(std::string(*layer.color_raster));
        p.width_ = static_cast<int>(p.precise_->width());
        p.height_ = static_cast<int>(p.precise_->height());
        p.channels_ = 4;
        p.hdr_ = p.precise_->floating();
        return p;
    }
    if (layer.ink_raster) {
        p.kind_ = Kind::Inks;
        p.inks_ = core::decode_ink_raster(*layer.ink_raster);
        if (!p.inks_.has_alpha()) return std::nullopt;
        p.width_ = static_cast<int>(p.inks_.width);
        p.height_ = static_cast<int>(p.inks_.height);
        p.channels_ = static_cast<int>(p.inks_.channels());
        p.hdr_ = p.inks_.is_float();
        return p;
    }
    if (layer.raster_png && !layer.raster_png->empty()) {
        p.kind_ = Kind::Eight;
        const Image picture = raster::ensure_raster(page, layer);
        p.width_ = picture.width();
        p.height_ = picture.height();
        p.bytes_ = picture.tobytes();
        p.channels_ = 4;
        return p;
    }
    return std::nullopt;
}

void OwnPixels::get(std::size_t i, float* v) const {
    switch (kind_) {
        case Kind::Eight:
            for (int c = 0; c < 4; ++c) v[c] = static_cast<unsigned char>(bytes_[i * 4 + std::size_t(c)]) / 255.0f;
            return;
        case Kind::Precise: {
            const auto px = precise_->pixel(i);
            for (int c = 0; c < 4; ++c) v[c] = static_cast<float>(px[std::size_t(c)]);
            return;
        }
        case Kind::Inks:
            for (int c = 0; c < channels_; ++c) v[c] = static_cast<float>(inks_.value(i, static_cast<unsigned>(c)));
            return;
    }
}

void OwnPixels::put(std::size_t i, const float* v) {
    changed_ = true;
    switch (kind_) {
        case Kind::Eight:
            for (int c = 0; c < 4; ++c) {
                const float x = std::isfinite(v[c]) ? std::clamp(v[c], 0.0f, 1.0f) : 0.0f;
                bytes_[i * 4 + std::size_t(c)] = static_cast<char>(std::lround(x * 255.0f));
            }
            return;
        case Kind::Precise: {
            std::array<double, 4> rgba{};
            for (int c = 0; c < 4; ++c) {
                const double x = std::isfinite(v[c]) ? static_cast<double>(v[c]) : 0.0;
                rgba[std::size_t(c)] = hdr_ && c < 3 ? std::max(x, 0.0) : std::clamp(x, 0.0, 1.0);
            }
            if (!(rgba[3] > 0.0)) rgba = {0.0, 0.0, 0.0, 0.0};
            precise_->set(i, rgba);
            return;
        }
        case Kind::Inks:
            for (int c = 0; c < channels_; ++c) inks_.set(i, static_cast<unsigned>(c), std::isfinite(v[c]) ? static_cast<double>(v[c]) : 0.0);
            return;
    }
}

resample::Plane OwnPixels::read(const Window& w) const {
    resample::Plane out(w.width(), w.height(), channels_, true);
    out.hdr = hdr_;
    for (int y = 0; y < w.height(); ++y)
        for (int x = 0; x < w.width(); ++x) get(index(w.x0 + x, w.y0 + y), out.at(x, y));
    return out;
}

std::optional<Window> OwnPixels::shown() const {
    Window b{width_, height_, 0, 0};
    std::vector<float> v(static_cast<std::size_t>(channels_));
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            get(index(x, y), v.data());
            if (!(v.back() > 0.0f)) continue;
            b.x0 = std::min(b.x0, x);
            b.y0 = std::min(b.y0, y);
            b.x1 = std::max(b.x1, x + 1);
            b.y1 = std::max(b.y1, y + 1);
        }
    }
    if (b.empty()) return std::nullopt;
    return b;
}

void OwnPixels::leave(const Window& w, const std::vector<float>* keep) {
    std::vector<float> v(static_cast<std::size_t>(channels_));
    for (int y = 0; y < w.height(); ++y) {
        for (int x = 0; x < w.width(); ++x) {
            const std::size_t i = index(w.x0 + x, w.y0 + y);
            get(i, v.data());
            const float k = keep != nullptr ? (*keep)[std::size_t(y) * std::size_t(w.width()) + std::size_t(x)] : 0.0f;
            if (k >= 1.0f || !(v.back() > 0.0f)) continue;
            v.back() *= k;
            put(i, v.data());
        }
    }
}

void OwnPixels::over(const resample::Plane& p, int x0, int y0) {
    std::vector<float> d(static_cast<std::size_t>(channels_));
    const int a = channels_ - 1;
    for (int y = 0; y < p.height; ++y) {
        const int gy = y0 + y;
        if (gy < 0 || gy >= height_) continue;
        for (int x = 0; x < p.width; ++x) {
            const int gx = x0 + x;
            if (gx < 0 || gx >= width_) continue;
            const float* t = p.at(x, y);
            const float ta = t[a];
            if (!(ta > 0.0f)) continue;
            const std::size_t i = index(gx, gy);
            get(i, d.data());
            const float da = d[std::size_t(a)];
            if (ta >= 1.0f || !(da > 0.0f)) {  // (over nothing, or opaque: its own values, exactly)
                std::copy_n(t, channels_, d.data());
                put(i, d.data());
                continue;
            }
            const float oa = ta + da * (1.0f - ta);
            for (int c = 0; c < a; ++c) d[std::size_t(c)] = oa > 0.0f ? (t[c] * ta + d[std::size_t(c)] * da * (1.0f - ta)) / oa : 0.0f;
            d[std::size_t(a)] = oa;
            put(i, d.data());
        }
    }
}

void OwnPixels::write(const resample::Plane& p, int x0, int y0) {
    for (int y = 0; y < p.height; ++y) {
        const int gy = y0 + y;
        if (gy < 0 || gy >= height_) continue;
        for (int x = 0; x < p.width; ++x) {
            const int gx = x0 + x;
            if (gx < 0 || gx >= width_) continue;
            put(index(gx, gy), p.at(x, y));
        }
    }
}

void OwnPixels::store(core::Page& page, core::Layer& layer) {
    if (!changed_) return;
    switch (kind_) {
        case Kind::Eight:
            raster::save_raster(page, layer, Image::frombytes("RGBA", Size{width_, height_}, bytes_));
            return;
        case Kind::Precise:
            layer.color_raster = std::make_shared<const std::string>(std::move(*precise_).take());
            return;
        case Kind::Inks:
            layer.ink_raster = std::make_shared<const std::string>(core::encode_ink_raster(inks_));
            layer.ink_memo = {};
            return;
    }
}

resample::Plane plane_of_picture(const Image& picture, bool& grey) {
    grey = picture.mode() == "L";
    if (!grey) return resample::from_rgba8(picture.mode() == "RGBA" ? picture : picture.convert("RGBA"));
    resample::Plane out(picture.width(), picture.height(), 2, true);
    const std::string bytes = picture.tobytes();
    for (int y = 0; y < picture.height(); ++y) {
        for (int x = 0; x < picture.width(); ++x) {
            float* v = out.at(x, y);
            v[0] = 1.0f;
            v[1] = static_cast<unsigned char>(bytes[std::size_t(y) * std::size_t(picture.width()) + std::size_t(x)]) / 255.0f;
        }
    }
    return out;
}

Image picture_of_plane(const resample::Plane& plane, bool grey) {
    if (!grey) return resample::to_rgba8(plane);
    std::string bytes(std::size_t(plane.width) * std::size_t(plane.height), '\0');
    for (int y = 0; y < plane.height; ++y)
        for (int x = 0; x < plane.width; ++x)
            bytes[std::size_t(y) * std::size_t(plane.width) + std::size_t(x)] = static_cast<char>(std::lround(std::clamp(plane.at(x, y)[1], 0.0f, 1.0f) * 255.0f));
    return Image::frombytes("L", Size{plane.width, plane.height}, bytes);
}

}  // namespace genko::render::own_pixels
