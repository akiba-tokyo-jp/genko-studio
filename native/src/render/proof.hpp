#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"
#include "render/image.hpp"

// 色の校正・校正の設定・色域外警告 (SPEC COLOR-03, PC1b; PhotoCraft v0.2.0's engine/src/proof_sim.rs and
// cms/src/gamut.rs, MIT: third_party/photocraft/NOTICE.md): how the page on screen is shown — as it will print
// through a proof profile, one plate of it, the page's numbers on another display, or as people with a colour vision
// deficiency see it — and which of its colours the proof profile cannot print. Display only: never the book, its
// history or what is exported. The picture given is the page as the screen shows it (sRGB, 8 bits).

namespace genko::render::proof {

// What a proof shows (upstream's ProofKind, with Photoshop's Internet Standard RGB and Monitor RGB).
enum class Kind {
    Profile,    // through the proof profile (作業用 CMYK, カスタム…)
    Plate,      // one plate of the proof profile (Setup::plate: 0 cyan, 1 magenta, 2 yellow, 3 black), as grey ink
    CmyPlate,   // cyan, magenta and yellow (no black)
    Srgb,       // インターネット標準の RGB: the page's numbers shown as sRGB
    Monitor,    // モニターの RGB: the page's numbers sent to the monitor as they are (no display conversion)
    LegacyMac,  // 旧 Macintosh の RGB: the page's numbers on a display of gamma 1.8 (sRGB primaries)
    Protan,     // P 型（1 型）色覚
    Deutan,     // D 型（2 型）色覚
};

// The kind's id as upstream names it (view.proofSetup.<id>; "profile" for Profile), and back (none: not one).
std::string kind_id(Kind kind, int plate = 3);
std::optional<std::pair<Kind, int>> kind_from_id(std::string_view id);

// 校正の設定 (upstream's ProofSetup).
struct Setup {
    Kind kind = Kind::Profile;
    int plate = 3;  // (Kind::Plate)
    // The proof profile (CMYK, RGB or grey ICC bytes). None: Genko's own conversion for print, as Python's 「CMYK で
    // 見る」 (render/colour::proof through `export_icc`, the CMYK profile chosen for export, or without one the plain
    // conversion: black on K alone, the total ink limited).
    core::Bytes icc;
    std::optional<std::filesystem::path> export_icc;
    std::string intent = "relative";  // to the proof profile (perceptual | relative | saturation | absolute)
    bool bpc = true;
    bool simulate_paper = false;  // the paper's white and the black ink shown (absolute colorimetric to the screen)

    bool operator==(const Setup& other) const;
};

// The page's own numbers (Srgb, Monitor, LegacyMac read them): what the screen's sRGB came from — an RGB page's
// profile (an RGB ICC), a grey page's (a grey ICC), or none (sRGB: the picture is its numbers).
struct Source {
    core::Bytes icc;
    bool gray = false;
    bool operator==(const Source& other) const;
};

// The picture ("RGB" or "RGBA", sRGB; "RGBA" keeps its alpha) as the proof shows it. core::Error("value") when the
// proof cannot be made (a plate of a profile that is not CMYK, a profile Little CMS cannot use).
Image apply(const Image& image, const Setup& setup, const Source& source = {});

// A CMYK page's plates as Kind::Plate and Kind::CmyPlate show them: its own inks (opaque, as render_page gives them
// with RenderOptions::inks) printed through its profile with black ink alone (one plate) or without the black.
Image page_plates(const core::InkPixels& inks, const core::Bytes& page_icc, const Setup& setup);

// Viénot, Brettel & Mollon (1999), "Digital video colourmaps for checking the legibility of displays by dichromats":
// a dichromat's view of a linear sRGB colour (0..1), as upstream's dichromat().
std::array<float, 3> dichromat(std::array<float, 3> linear, bool protan);

// 色域外警告 (upstream's GamutCheck): a colour is out of the proof profile's gamut when going to it and back to Lab
// (relative colorimetric, no black point compensation, the device values clipped) moves it by more than the threshold
// (CIE ΔE 1976). The ΔE is tabulated over sRGB on a 33³ grid and read between its points (trilinear).
inline constexpr double kDefaultThreshold = 4.0;

class Gamut {
public:
    // The check against the setup's proof profile (its ICC; none: the export profile, or the plain conversion's round
    // trip). Made once for each profile (kept). core::Error("value") when the profile cannot be used.
    static std::shared_ptr<const Gamut> of(const Setup& setup);

    // ΔE76 between an sRGB colour (0..255) and its round trip.
    float delta_e(int r, int g, int b) const;
    // Each pixel of the picture ("RGB" or "RGBA"; a transparent pixel never is): 255 out of gamut, else 0 — one byte a
    // pixel, row by row — and how many are.
    std::vector<unsigned char> mask(const Image& image, double threshold, std::size_t* out = nullptr) const;

    static constexpr int kGrid = 33;

private:
    std::vector<float> grid_;  // (r fastest)
    friend struct GamutMaker;
};

// カラー設定's policies for a picture taken in (SPEC COLOR-03 PC1b): its own ICC profile (PNG iCCP, JPEG APP2
// ICC_PROFILE, TIFF tag 34675, WebP ICCP, PSD image resource 1039), or none.
std::optional<std::string> embedded_profile(std::string_view bytes);
// A profile that shows the picture's numbers as Genko takes them anyway (an sRGB one; a grey one whose grey v is sRGB's
// (v, v, v)): within half an 8-bit step on a lattice. (CMYK: never.)
bool same_as_srgb(std::string_view icc);
// The picture ("RGB", "RGBA", "P", "L", "LA", "1", "CMYK"…) taken from the profile (of its colours' space) to sRGB:
// "RGB", or "RGBA" keeping its alpha. core::Error("value") when the profile is not of its colours.
Image convert_picture(const Image& picture, std::string_view icc, std::string_view intent, bool bpc);

}  // namespace genko::render::proof
