#include "render/color_canvas.hpp"
#include "core/strokes.hpp"
#include <lcms2.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <mutex>
#include <vector>
#include "core/error.hpp"
#include "core/model.hpp"
#include "core/filters.hpp"
#include "core/pyconv.hpp"
#include "core/pyops.hpp"
#include "render/imaging.hpp"
#include "render/not_yet_ported.hpp"

namespace genko::render {

// --- RgbSpace ----------------------------------------------------------------------------------------------------------

namespace {

// A profile's tone curve in double precision when it is one of the ICC parametric functions this reads analytically
// (Little CMS's types 1, 4 and 5: a gamma, IEC 61966-2.1's and its offset form): encoded → linear and back. Values past
// 0..1 (HDR) follow the same formula (a gamma mirrored below 0).
struct Parametric {
    int type = 0;  // (0: not one of them)
    std::array<double, 7> p{};

    explicit Parametric(const cmsToneCurve* curve) {
        const int t = cmsGetToneCurveParametricType(curve);
        if (t != 1 && t != 4 && t != 5) return;
        const cmsFloat64Number* params = cmsGetToneCurveParams(curve);
        if (params == nullptr) return;
        for (std::size_t i = 0; i < p.size(); ++i) p[i] = params[i];
        type = t;
        if ((type == 4 || type == 5) && (p[1] <= 0 || p[0] <= 0 || p[3] <= 0)) type = 0;  // (a, g, c: no inverse)
    }
    double linear(double x) const {
        const double g = p[0];
        if (type == 1) return x < 0 ? -std::pow(-x, g) : std::pow(x, g);
        const double a = p[1], b = p[2], c = p[3], d = p[4], e = type == 5 ? p[5] : 0.0, f = type == 5 ? p[6] : 0.0;
        if (x >= d) return std::pow(std::max(0.0, a * x + b), g) + e;
        return c * x + f;
    }
    double encoded(double y) const {
        const double g = p[0];
        if (type == 1) return y < 0 ? -std::pow(-y, 1.0 / g) : std::pow(y, 1.0 / g);
        const double a = p[1], b = p[2], c = p[3], d = p[4], e = type == 5 ? p[5] : 0.0, f = type == 5 ? p[6] : 0.0;
        const double knee = std::pow(std::max(0.0, a * d + b), g) + e;
        if (y >= knee) return (std::pow(std::max(0.0, y - e), 1.0 / g) - b) / a;
        return (y - f) / c;
    }
};

}  // namespace

struct RgbSpace::Impl {
    std::array<cmsToneCurve*, 3> curves{};    // (encoded → linear)
    std::array<cmsToneCurve*, 3> reversed{};  // (linear → encoded)
    std::vector<Parametric> exact;            // (the same in double precision, when they are parametric)
    cmsHTRANSFORM in = nullptr;                // sRGB → the page (its intent, black point compensation)
    cmsHTRANSFORM out = nullptr;               // the page → sRGB (relative colorimetric, black point compensation)
    ~Impl() {
        for (cmsToneCurve* c : curves) if (c != nullptr) cmsFreeToneCurve(c);
        for (cmsToneCurve* c : reversed) if (c != nullptr) cmsFreeToneCurve(c);
        if (in != nullptr) cmsDeleteTransform(in);
        if (out != nullptr) cmsDeleteTransform(out);
    }
};

RgbSpace::RgbSpace(const core::RgbMode& mode) {
    if (mode.profile.empty()) return;  // (sRGB: the canvas's own formulas)
    if (!mode.icc) throw core::Error("value", "the page's RGB profile " + mode.profile + " is not held");
    // (made once for a profile, an intent and black point compensation: every render of the page uses them)
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<const Impl>> made;
    const std::string key = mode.profile + ":" + mode.intent + (mode.bpc ? "" : ":nobpc");
    const std::lock_guard<std::mutex> held(lock);
    if (const auto it = made.find(key); it != made.end()) {
        impl_ = it->second;
        return;
    }
    cmsHPROFILE page = cmsOpenProfileFromMem(mode.icc->data(), static_cast<cmsUInt32Number>(mode.icc->size()));
    if (page == nullptr || !cmsIsMatrixShaper(page)) {
        if (page != nullptr) cmsCloseProfile(page);
        throw core::Error("value", "the page's RGB profile is not a matrix-shaper profile");
    }
    auto impl = std::make_shared<Impl>();
    const std::array<cmsTagSignature, 3> tags{cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
    for (unsigned c = 0; c < 3; ++c) {
        const auto* curve = static_cast<const cmsToneCurve*>(cmsReadTag(page, tags[c]));
        if (curve == nullptr) {
            cmsCloseProfile(page);
            throw core::Error("value", "the page's RGB profile has no tone curves");
        }
        impl->curves[c] = cmsDupToneCurve(curve);
        impl->reversed[c] = cmsReverseToneCurve(curve);
        impl->exact.emplace_back(curve);
    }
    cmsHPROFILE srgb = cmsCreate_sRGBProfile();
    const auto intent = static_cast<cmsUInt32Number>(std::find(core::kIntents.begin(), core::kIntents.end(), mode.intent) - core::kIntents.begin());
    impl->in = cmsCreateTransform(srgb, TYPE_RGB_DBL, page, TYPE_RGB_DBL, intent,
                                  (mode.bpc ? cmsFLAGS_BLACKPOINTCOMPENSATION : 0) | cmsFLAGS_NOCACHE);
    impl->out = cmsCreateTransform(page, TYPE_RGB_DBL, srgb, TYPE_RGB_DBL, INTENT_RELATIVE_COLORIMETRIC,
                                   cmsFLAGS_BLACKPOINTCOMPENSATION | cmsFLAGS_NOCACHE);
    cmsCloseProfile(srgb);
    cmsCloseProfile(page);
    if (impl->in == nullptr || impl->out == nullptr) throw core::Error("value", "the page's RGB profile cannot be used");
    impl_ = impl;
    made.emplace(key, std::move(impl));
}

RgbSpace RgbSpace::of(const core::Page& page) { return page.rgb ? RgbSpace(*page.rgb) : RgbSpace(); }

double RgbSpace::decode(unsigned channel, double encoded) const {
    if (!impl_) return core::srgb_to_linear(encoded);
    if (impl_->exact[channel].type != 0) return impl_->exact[channel].linear(encoded);
    return cmsEvalToneCurveFloat(impl_->curves[channel], static_cast<cmsFloat32Number>(encoded));
}

double RgbSpace::encode(unsigned channel, double linear) const {
    if (!impl_) return core::linear_to_srgb(linear);
    if (impl_->exact[channel].type != 0) return impl_->exact[channel].encoded(linear);
    return cmsEvalToneCurveFloat(impl_->reversed[channel], static_cast<cmsFloat32Number>(linear));
}

void RgbSpace::from_srgb(double* rgb, std::size_t count) const {
    if (!impl_ || count == 0) return;
    // (an sRGB colour in 0..1 stays in 0..1: what the profiles' fixed-point matrices leave past it, sRGB's white a few
    // millionths over 1 in Display P3, is their rounding; so is what they leave of sRGB's white and black — the white
    // of paper — a few millionths short of the page's own: snapped to it)
    enum class Kind : unsigned char { Outside, Inside, White, Black };
    std::vector<Kind> kinds(count);
    for (std::size_t i = 0; i < count; ++i) {
        const double* p = rgb + i * 3;
        kinds[i] = p[0] == 1.0 && p[1] == 1.0 && p[2] == 1.0   ? Kind::White
                   : p[0] == 0.0 && p[1] == 0.0 && p[2] == 0.0 ? Kind::Black
                   : std::all_of(p, p + 3, [](double v) { return v >= 0.0 && v <= 1.0; }) ? Kind::Inside
                                                                                           : Kind::Outside;
    }
    cmsDoTransform(impl_->in, rgb, rgb, static_cast<cmsUInt32Number>(count));
    constexpr double kNear = 1e-4;
    for (std::size_t i = 0; i < count; ++i) {
        if (kinds[i] == Kind::Outside) continue;
        double* p = rgb + i * 3;
        if (kinds[i] == Kind::White && std::all_of(p, p + 3, [](double v) { return std::abs(v - 1.0) < kNear; })) {
            std::fill(p, p + 3, 1.0);
        } else if (kinds[i] == Kind::Black && std::all_of(p, p + 3, [](double v) { return std::abs(v) < kNear; })) {
            std::fill(p, p + 3, 0.0);
        } else {
            for (unsigned c = 0; c < 3; ++c) p[c] = std::clamp(p[c], 0.0, 1.0);
        }
    }
}

void RgbSpace::to_srgb(double* rgb, std::size_t count) const {
    if (!impl_ || count == 0) return;
    cmsDoTransform(impl_->out, rgb, rgb, static_cast<cmsUInt32Number>(count));
}

namespace {
struct Work {
    ImagingMemoryInstance reservation{};
    ~Work() { genko_imaging_budget_release(&reservation); }
};
using Pixel = std::array<double, 5>; // straight linear RGB, alpha, preceding layer alpha
using RGB = std::array<double,3>;
// PhotoCraft color/blend.rs scalar formulas, evaluated without an RGBA8 intermediate.
// The existing RGB8/Python compositor is deliberately unchanged (its float32 and Soft Light semantics differ).
double luminosity(const RGB& c) { return .3*c[0]+.59*c[1]+.11*c[2]; }
RGB set_luminosity(RGB c,double target) {
    const double delta=target-luminosity(c);for(auto& v:c)v+=delta;
    const double l=luminosity(c),lo=*std::min_element(c.begin(),c.end()),hi=*std::max_element(c.begin(),c.end());
    if(lo<0)for(auto& v:c)v=std::abs(l-lo)<1e-12?l:l+(v-l)*l/(l-lo);
    if(hi>1)for(auto& v:c)v=std::abs(hi-l)<1e-12?l:l+(v-l)*(1-l)/(hi-l);
    return c;
}
double saturation(const RGB& c) { return *std::max_element(c.begin(),c.end())-*std::min_element(c.begin(),c.end()); }
RGB set_saturation(RGB c,double target) {
    const double lo=*std::min_element(c.begin(),c.end()),span=saturation(c);
    for(auto& v:c)v=span>0?(v-lo)*target/span:0;
    return c;
}
double color_burn(double b,double s) { return b>=1?1:s<=0?0:1-std::min(1.0,(1-b)/s); }
double color_dodge(double b,double s) { return b<=0?0:s>=1?1:std::min(1.0,b/(1-s)); }
RGB blend_rgb(std::string_view mode,const RGB& b,const RGB& s) {
    if(mode=="hue")return set_luminosity(set_saturation(s,saturation(b)),luminosity(b));
    if(mode=="saturation")return set_luminosity(set_saturation(b,saturation(s)),luminosity(b));
    if(mode=="color")return set_luminosity(s,luminosity(b));
    if(mode=="luminosity")return set_luminosity(b,luminosity(s));
    RGB out{};
    for(unsigned c=0;c<3;++c){const double cb=b[c],cs=s[c];
        if(mode=="multiply")out[c]=cb*cs;
        else if(mode=="screen")out[c]=cb+cs-cb*cs;
        else if(mode=="overlay")out[c]=cb<=.5?2*cb*cs:1-2*(1-cb)*(1-cs);
        else if(mode=="add")out[c]=std::min(1.0,cb+cs);
        else if(mode=="darken")out[c]=std::min(cb,cs);
        else if(mode=="lighten")out[c]=std::max(cb,cs);
        else if(mode=="color_burn")out[c]=color_burn(cb,cs);
        else if(mode=="color_dodge")out[c]=color_dodge(cb,cs);
        else if(mode=="linear_burn")out[c]=std::max(0.0,cb+cs-1);
        else if(mode=="soft_light")out[c]=cs<=.5?2*cb*cs+cb*cb*(1-2*cs):2*cb*(1-cs)+std::sqrt(std::max(0.0,cb))*(2*cs-1);
        else if(mode=="hard_light")out[c]=cs<=.5?2*cb*cs:cb+(2*cs-1)-cb*(2*cs-1);
        else if(mode=="difference")out[c]=std::abs(cb-cs);
        else if(mode=="exclusion")out[c]=cb+cs-2*cb*cs;
        else if(mode=="subtract")out[c]=std::max(0.0,cb-cs);
        else if(mode=="divide")out[c]=cs<=0?(cb<=0?0:1):std::min(1.0,cb/cs);
        else out[c]=cs; // normal (unknown modes are refused at the public entry)
    }
    return out;
}
std::array<double, 4> linear(std::array<double, 4> p) {
    for (unsigned c = 0; c < 3; ++c) p[c] = core::srgb_to_linear(p[c]);
    return p;
}
// The space's encoded values (straight RGBA) in linear light.
std::array<double, 4> linear(const RgbSpace& space, std::array<double, 4> p) {
    if (space.is_srgb()) return linear(p);
    for (unsigned c = 0; c < 3; ++c) p[c] = space.decode(c, p[c]);
    return p;
}
// An sRGB colour (straight RGBA, encoded) in the space's linear light (on a page of a profile: into it first).
std::array<double, 4> linear_srgb(const RgbSpace& space, std::array<double, 4> p) {
    if (space.is_srgb()) return linear(p);
    space.from_srgb(p.data(), 1);
    for (unsigned c = 0; c < 3; ++c) p[c] = space.decode(c, p[c]);
    return p;
}
int byte(double x) { return static_cast<int>(std::lround(std::clamp(x, 0.0, 1.0)*255)); }
// 8-bit sRGB colours into a space's linear light as linear_srgb brings them (the same values), each colour once: a row's
// colours not met yet go through Little CMS together, the rest are found by their 24 bits (a page's pictures have few
// colours where most pixels are: the paper, flat paint, the clear parts).
class SrgbRows {
public:
    explicit SrgbRows(const RgbSpace& space) : space_(space) {}
    // A row of `n` RGBA pixels (bytes) as straight linear RGBA.
    void row(const unsigned char* rgba, std::size_t n, std::vector<std::array<double, 4>>& out) {
        out.resize(n);
        if (space_.is_srgb()) {
            for (std::size_t x = 0; x < n; ++x)
                out[x] = linear({rgba[x*4]/255.0, rgba[x*4+1]/255.0, rgba[x*4+2]/255.0, rgba[x*4+3]/255.0});
            return;
        }
        if (known_.size() > (1u << 20)) known_.clear();  // (a picture of very many colours: kept to a bound)
        pending_.clear();
        for (std::size_t x = 0; x < n; ++x) {
            const std::uint32_t key = (std::uint32_t(rgba[x*4]) << 16) | (std::uint32_t(rgba[x*4+1]) << 8) | rgba[x*4+2];
            if (known_.try_emplace(key, std::array<double, 3>{-1, -1, -1}).second) pending_.push_back(key);
        }
        if (!pending_.empty()) {
            std::vector<double> rgb(pending_.size() * 3);
            for (std::size_t k = 0; k < pending_.size(); ++k)
                for (unsigned c = 0; c < 3; ++c) rgb[k*3+c] = ((pending_[k] >> (16 - 8*c)) & 255) / 255.0;
            space_.from_srgb(rgb.data(), pending_.size());
            for (std::size_t k = 0; k < pending_.size(); ++k) {
                auto& made = known_[pending_[k]];
                for (unsigned c = 0; c < 3; ++c) made[c] = space_.decode(c, rgb[k*3+c]);
            }
        }
        for (std::size_t x = 0; x < n; ++x) {
            const auto& c = known_.find((std::uint32_t(rgba[x*4]) << 16) | (std::uint32_t(rgba[x*4+1]) << 8) | rgba[x*4+2])->second;
            out[x] = {c[0], c[1], c[2], rgba[x*4+3]/255.0};
        }
    }

private:
    const RgbSpace& space_;
    std::unordered_map<std::uint32_t, std::array<double, 3>> known_;
    std::vector<std::uint32_t> pending_;
};
std::array<double, 4> sampled(const core::ColorRasterView& src, double x, double y, const RgbSpace& space) {
    x = std::clamp(x, 0.0, double(src.width()-1));
    y = std::clamp(y, 0.0, double(src.height()-1));
    const auto x0 = static_cast<std::uint32_t>(x), y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0+1, src.width()-1), y1 = std::min(y0+1, src.height()-1);
    const double fx = x-x0, fy = y-y0;
    const std::array<double, 4> weights{(1-fx)*(1-fy), fx*(1-fy), (1-fx)*fy, fx*fy};
    const std::array<std::size_t, 4> indices{std::size_t(y0)*src.width()+x0, std::size_t(y0)*src.width()+x1,
        std::size_t(y1)*src.width()+x0, std::size_t(y1)*src.width()+x1};
    std::array<double, 4> out{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto p = linear(space, src.pixel(indices[i]));
        out[3] += p[3]*weights[i];
        for (unsigned c = 0; c < 3; ++c) out[c] += p[c]*p[3]*weights[i];
    }
    if (out[3] > 0) for (unsigned c = 0; c < 3; ++c) out[c] /= out[3];
    return out;
}
}
struct ColorCanvas::Impl {
    // The budget must outlive both the reservation and its payload.
    ImageAllocationBudget budget{512ULL*1024*1024};
    Work work;
    std::vector<Pixel> pixels;
    Size size;
    RgbSpace space;
    bool previous = false;
    Impl(Size s, RgbSpace rgb) : size(s), space(std::move(rgb)) {
        if (s.width <= 0 || s.height <= 0) throw core::Error("value", "invalid color canvas dimensions");
        const auto n = std::uint64_t(s.width)*std::uint64_t(s.height);
        if (n > (512ULL*1024*1024)/sizeof(Pixel)) throw core::Error("image_too_large", "color canvas budget exceeded");
        if (!genko_imaging_budget_reserve(&work.reservation, n*sizeof(Pixel))) detail::throw_imaging_error();
        pixels.resize(static_cast<std::size_t>(n));
    }
    void over(std::size_t i, const std::array<double, 4>& src, double opacity, bool clip, std::string_view mode) {
        auto& dst = pixels[i];
        const double a = src[3]*std::clamp(opacity, 0.0, 1.0)*(clip && previous ? dst[4] : 1);
        const bool normal=mode.empty() || mode=="normal";
        // Never evaluate zero-weight colour terms: HDR multiplication can overflow even when invisible.
        if(a==0){dst[4]=src[3];return;}
        if((normal && a==1) || dst[3]==0){
            for(unsigned c=0;c<3;++c){dst[c]=src[c];}dst[3]=a;dst[4]=src[3];return;
        }
        const double alpha=a+dst[3]*(1-a);
        const RGB mixed=normal?RGB{}:blend_rgb(mode,{dst[0],dst[1],dst[2]},{src[0],src[1],src[2]});
        RGB next{};
        if(alpha>0)for(unsigned c=0;c<3;++c){
            if(normal)next[c]=(src[c]*a+dst[c]*dst[3]*(1-a))/alpha;
            else next[c]=(dst[c]*dst[3]*(1-a)+src[c]*a*(1-dst[3])+mixed[c]*a*dst[3])/alpha;
            if(!std::isfinite(next[c]))throw core::Error("value","high-precision composite exceeds finite working range");
        }
        for(unsigned c=0;c<3;++c)dst[c]=next[c];
        dst[3]=alpha;dst[4]=src[3];
    }
};
ColorCanvas::ColorCanvas(const Image& initial, RgbSpace space) : impl_(std::make_unique<Impl>(initial.size(), std::move(space))) {
    const auto rgba = initial.convert("RGBA");
    SrgbRows in(impl_->space);  // (the paper: sRGB colours, into the page's space)
    std::vector<std::array<double, 4>> line;
    for (int y = 0; y < rgba.height(); ++y) {
        in.row(reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]), static_cast<std::size_t>(rgba.width()), line);
        for (int x = 0; x < rgba.width(); ++x) {
            const auto& p = line[x];
            impl_->pixels[std::size_t(y)*rgba.width()+x] = {p[0], p[1], p[2], p[3], 0};
        }
    }
}
ColorCanvas::~ColorCanvas() = default;
bool ColorCanvas::supports_blend(std::string_view mode) {
    static constexpr std::array<std::string_view,20> modes={"normal","multiply","screen","add","overlay","darken","lighten",
        "color_burn","color_dodge","linear_burn","soft_light","hard_light","difference","exclusion","subtract","divide",
        "hue","saturation","color","luminosity"};
    return mode.empty() || std::find(modes.begin(),modes.end(),mode)!=modes.end();
}
void ColorCanvas::blend(const Image& input, double opacity, bool clip, std::string_view mode, bool page_values) {
    if(!supports_blend(mode))throw core::Error("not_yet_ported","high-precision blend:"+std::string(mode));
    if (input.size() != impl_->size) throw core::Error("value", "color canvas images do not match");
    const auto rgba = input.convert("RGBA");
    const RgbSpace& space = impl_->space;
    SrgbRows in(space);
    std::vector<std::array<double, 4>> line;
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const unsigned char*>(rgba.raw()->image[y]);
        if (page_values) {
            for (int x = 0; x < rgba.width(); ++x) {
                const std::array<double, 4> p{row[x*4]/255.0, row[x*4+1]/255.0, row[x*4+2]/255.0, row[x*4+3]/255.0};
                impl_->over(std::size_t(y)*rgba.width()+x, linear(space, p), opacity, clip, mode);
            }
            continue;
        }
        in.row(row, static_cast<std::size_t>(rgba.width()), line);
        for (int x = 0; x < rgba.width(); ++x) impl_->over(std::size_t(y)*rgba.width()+x, line[x], opacity, clip, mode);
    }
    impl_->previous = true;
}
void ColorCanvas::blend(const core::ColorRasterView& source, Size full, Box area, double opacity, bool clip, std::string_view mode,
                        const Image* mask) {
    if(!supports_blend(mode))throw core::Error("not_yet_ported","high-precision blend:"+std::string(mode));
    if (full.width <= 0 || full.height <= 0 || area.width() != impl_->size.width || area.height() != impl_->size.height)
        throw core::Error("value", "color canvas region does not match");
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "layer mask does not match");
    for (int y = 0; y < area.height(); ++y) {
        const auto* shown = mask ? reinterpret_cast<const unsigned char*>(mask->raw()->image[y]) : nullptr;
        for (int x = 0; x < area.width(); ++x) {
            auto p = sampled(source, (area.x0+x+.5)*source.width()/full.width-.5,
                (area.y0+y+.5)*source.height()/full.height-.5, impl_->space);
            if (shown) p[3] *= shown[x]/255.0;
            impl_->over(std::size_t(y)*area.width()+x, p, opacity, clip, mode);
        }
    }
    impl_->previous = true;
}
void ColorCanvas::blend(const ColorCanvas& source, double opacity, bool clip, std::string_view mode) {
    if (!supports_blend(mode) || source.impl_->size != impl_->size) throw core::Error("value","color canvas source does not match");
    for (std::size_t i=0;i<impl_->pixels.size();++i) {
        const auto& p=source.impl_->pixels[i];
        impl_->over(i,{p[0],p[1],p[2],p[3]},opacity,clip,mode);
    }
    impl_->previous=true;
}
void ColorCanvas::blend_stroke(const Image& mask, const core::Json& color, double opacity) {
    core::validate_stroke_color(color);
    if (mask.mode() != "L" || mask.size() != impl_->size) throw core::Error("value","stroke coverage does not match");
    auto p=linear_srgb(impl_->space,{color["values"][0].get<double>(),color["values"][1].get<double>(),color["values"][2].get<double>(),1});
    for (int y=0;y<mask.height();++y) {
        const auto* row=reinterpret_cast<const unsigned char*>(mask.raw()->image[y]);
        for (int x=0;x<mask.width();++x) { p[3]=row[x]/255.;impl_->over(std::size_t(y)*mask.width()+x,p,opacity,false,"normal"); }
    }
    impl_->previous=true;
}
void ColorCanvas::expose(const core::Exposure& e, double opacity, bool clip, const Image* mask, std::string_view mode) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "exposure mask does not match");
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "high-precision blend:" + std::string(mode));
    const bool normal = mode.empty() || mode == "normal";
    const RgbSpace& space = impl_->space;
    for (int y = 0; y < impl_->size.height; ++y) for (int x = 0; x < impl_->size.width; ++x) {
        auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
        double amount = std::clamp(opacity, 0.0, 1.0)*(clip && impl_->previous ? p[4] : 1);
        if (mask) amount *= reinterpret_cast<const unsigned char*>(mask->raw()->image[y])[x]/255.0;
        if (normal) {
            for (unsigned c = 0; c < 3; ++c) p[c] = std::lerp(p[c], e.apply(p[c]), amount);
            continue;
        }
        if (amount == 0) continue;
        // (PC2: in a blend, the exposed colour laid over the one below in their encoded samples, as adjust)
        const RGB was{space.encode(0, p[0]), space.encode(1, p[1]), space.encode(2, p[2])};
        const RGB exposed{space.encode(0, e.apply(p[0])), space.encode(1, e.apply(p[1])), space.encode(2, e.apply(p[2]))};
        const RGB mixed = blend_rgb(mode, was, exposed);
        for (unsigned c = 0; c < 3; ++c) {
            p[c] = space.decode(c, std::lerp(was[c], mixed[c], amount));
            if (!std::isfinite(p[c])) throw core::Error("value", "high-precision adjustment exceeds finite working range");
        }
    }
}
void ColorCanvas::adjust(const core::AdjustFunction& adjustment, double opacity, bool clip, const Image* mask, std::string_view mode, Point at) {
    if (mask && (mask->mode() != "L" || mask->size() != impl_->size)) throw core::Error("value", "adjustment mask does not match");
    if (!supports_blend(mode)) throw core::Error("not_yet_ported", "high-precision blend:" + std::string(mode));
    const bool normal = mode.empty() || mode == "normal";
    for (int y = 0; y < impl_->size.height; ++y) for (int x = 0; x < impl_->size.width; ++x) {
        auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
        double amount = std::clamp(opacity, 0.0, 1.0)*(clip && impl_->previous ? p[4] : 1);
        if (mask) amount *= reinterpret_cast<const unsigned char*>(mask->raw()->image[y])[x]/255.0;
        if (amount == 0) continue;
        const RgbSpace& space = impl_->space;
        const std::array<double, 3> was{space.encode(0, p[0]), space.encode(1, p[1]), space.encode(2, p[2])};
        const auto changed = adjustment.apply(was, at.x + x, at.y + y);
        // (PC2, the user's decision 2: its native.blend — the changed colour over the one below by this canvas's formula)
        const auto laid = normal ? changed : blend_rgb(mode, was, changed);
        for (unsigned c = 0; c < 3; ++c) {
            p[c] = space.decode(c, std::lerp(was[c], laid[c], amount));
            if (!std::isfinite(p[c])) throw core::Error("value", "high-precision adjustment exceeds finite working range");
        }
    }
}
void ColorCanvas::draw_8bit(const std::function<void(Image&)>& draw) {
    const Image before = image();
    Image drawn = before;
    draw(drawn);
    Image probe = before;
    for (int y = 0; y < probe.height(); ++y) {
        auto* row = reinterpret_cast<unsigned char*>(probe.raw()->image[y]);
        for (int x = 0; x < probe.width(); ++x)
            for (unsigned c = 0; c < 3; ++c) row[x*4+c] = static_cast<unsigned char>((row[x*4+c] + 128) & 255);
    }
    const Image probe_before = probe;
    draw(probe);
    for (const Image* out : {&drawn, &probe})
        if (out->mode() != "RGBA" || out->size() != impl_->size)
            throw core::Error("value", "8-bit drawing does not match the color canvas");
    SrgbRows in(impl_->space);
    std::vector<std::array<double, 4>> line;
    for (int y = 0; y < impl_->size.height; ++y) {
        const auto* was = reinterpret_cast<const unsigned char*>(before.raw()->image[y]);
        const auto* now = reinterpret_cast<const unsigned char*>(drawn.raw()->image[y]);
        const auto* probe_was = reinterpret_cast<const unsigned char*>(probe_before.raw()->image[y]);
        const auto* probe_now = reinterpret_cast<const unsigned char*>(probe.raw()->image[y]);
        bool converted = false;
        for (int x = 0; x < impl_->size.width; ++x) {
            const auto* d = now + x*4;
            if (std::equal(d, d+4, was + x*4) && std::equal(probe_now + x*4, probe_now + x*4 + 4, probe_was + x*4)) continue;
            if (!converted) {  // (the row's colours, once a pixel of it is painted)
                in.row(now, static_cast<std::size_t>(impl_->size.width), line);
                converted = true;
            }
            auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
            for (unsigned c = 0; c < 4; ++c) p[c] = line[x][c];  // (p[4], the alpha of the layer before, stays)
        }
    }
}
Image ColorCanvas::image() const {
    auto out = Image::create("RGBA", impl_->size, Ink{0, 0, 0, 0});
    if (!impl_->space.is_srgb()) {  // (the page's values, encoded, as sRGB for the screen)
        const RgbSpace& space = impl_->space;
        std::vector<double> row(static_cast<std::size_t>(impl_->size.width) * 3);
        for (int y = 0; y < impl_->size.height; ++y) {
            for (int x = 0; x < impl_->size.width; ++x) {
                const auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
                for (unsigned c = 0; c < 3; ++c) {
                    row[x*3+c] = space.encode(c, p[c]);
                    if (!std::isfinite(row[x*3+c])) throw core::Error("value","high-precision output exceeds finite working range");
                }
            }
            space.to_srgb(row.data(), static_cast<std::size_t>(impl_->size.width));
            auto* dst = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
            for (int x = 0; x < impl_->size.width; ++x) {
                for (unsigned c = 0; c < 3; ++c) dst[x*4+c] = static_cast<unsigned char>(byte(row[x*3+c]));
                dst[x*4+3] = static_cast<unsigned char>(byte(impl_->pixels[std::size_t(y)*impl_->size.width+x][3]));
            }
        }
        return out;
    }
    for (int y = 0; y < impl_->size.height; ++y) {
        auto* row = reinterpret_cast<unsigned char*>(out.raw()->image[y]);
        for (int x = 0; x < impl_->size.width; ++x) {
            const auto& p = impl_->pixels[std::size_t(y)*impl_->size.width+x];
            for (unsigned c = 0; c < 3; ++c) {
                const auto encoded=core::linear_to_srgb(p[c]);
                if(!std::isfinite(encoded))throw core::Error("value","high-precision output exceeds finite working range");
                row[x*4+c]=static_cast<unsigned char>(byte(encoded));
            }
            row[x*4+3] = static_cast<unsigned char>(byte(p[3]));
        }
    }
    return out;
}
Size ColorCanvas::size() const { return impl_->size; }
const RgbSpace& ColorCanvas::space() const { return impl_->space; }
std::array<double, 4> ColorCanvas::linear_pixel(std::size_t i) const {
    if (i >= impl_->pixels.size()) throw core::Error("value", "color canvas pixel out of range");
    const auto& p = impl_->pixels[i];
    return {p[0], p[1], p[2], p[3]};
}
bool ColorCanvas::is_opaque() const {
    // (a raster stretched over the page is resampled: an opaque one's weights sum to 1 within rounding)
    return std::all_of(impl_->pixels.begin(), impl_->pixels.end(), [](const Pixel& p) { return p[3] >= 1 - 1e-12; });
}
bool ColorCanvas::keeps_preceding_alpha() const {
    return std::all_of(impl_->pixels.begin(), impl_->pixels.end(), [](const Pixel& p) { return p[3] == p[4]; });
}
std::string ColorCanvas::color_raster(std::string_view precision) const {
    return core::encode_color_pixels(static_cast<std::uint32_t>(impl_->size.width),
                                    static_cast<std::uint32_t>(impl_->size.height), precision,
        [this, u16 = precision == "u16"](std::size_t i) {
            // (a blend or a correction of values in 0..1 may land an ulp outside: back to the end it came from)
            const auto snap = [](double v) { return v < 0 && v > -1e-9 ? 0.0 : v > 1 && v < 1 + 1e-9 ? 1.0 : v; };
            const auto& p = impl_->pixels[i];
            const RgbSpace& space = impl_->space;
            std::array<double, 4> out{space.encode(0, p[0]), space.encode(1, p[1]), space.encode(2, p[2]), snap(p[3])};
            if (u16) for (int c = 0; c < 3; ++c) out[c] = snap(out[c]);
            return out;
        });
}
core::AdjustPtr correction_of(const core::Layer& layer, const core::Page& page, const core::Document* doc) {
    if (!layer.adjust || !layer.adjust->is_object()) return nullptr;
    // (another kind: as apply_filter's "unknown filter", the layer does nothing; a plugin and the filters that move shapes
    // are the callers' and validate_color_document's)
    try {
        return core::adjust_function(*layer.adjust, page, doc);
    } catch (const core::Error& error) {
        if (error.code() != "not_yet_ported") throw;
        const core::Json kind = layer.adjust->value("kind", core::Json());
        throw NotYetPorted("adjust:" + (kind.is_string() ? kind.get<std::string>() : core::py_str(kind)));
    }
}
Image srgb_into(const Image& rgba, const RgbSpace& space) {
    if (space.is_srgb()) return rgba;
    std::string bytes = rgba.convert("RGBA").tobytes();
    std::vector<double> rgb(bytes.size() / 4 * 3);
    for (std::size_t i = 0; i < bytes.size() / 4; ++i)
        for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0;
    space.from_srgb(rgb.data(), bytes.size() / 4);
    for (std::size_t i = 0; i < bytes.size() / 4; ++i)
        for (unsigned c = 0; c < 3; ++c) bytes[i * 4 + c] = static_cast<char>(byte(rgb[i * 3 + c]));
    return Image::frombytes("RGBA", rgba.size(), bytes);
}
Image srgb_from(const Image& rgba, const RgbSpace& space) {
    if (space.is_srgb()) return rgba;
    std::string bytes = rgba.convert("RGBA").tobytes();
    std::vector<double> rgb(bytes.size() / 4 * 3);
    for (std::size_t i = 0; i < bytes.size() / 4; ++i)
        for (unsigned c = 0; c < 3; ++c) rgb[i * 3 + c] = static_cast<unsigned char>(bytes[i * 4 + c]) / 255.0;
    space.to_srgb(rgb.data(), bytes.size() / 4);
    for (std::size_t i = 0; i < bytes.size() / 4; ++i)
        for (unsigned c = 0; c < 3; ++c) bytes[i * 4 + c] = static_cast<char>(byte(rgb[i * 3 + c]));
    return Image::frombytes("RGBA", rgba.size(), bytes);
}
Image color_raster_preview(const core::ColorRasterView& src, Size full, Box area, const RgbSpace& space) {
    const auto transparent = Image::create("RGBA", Size{area.width(), area.height()}, Ink{0, 0, 0, 0});
    ColorCanvas out(transparent, space);
    out.blend(src, full, area, 1, false);
    return out.image();
}
} // namespace genko::render
