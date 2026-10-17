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
#include "core/json.hpp"
#include "core/model.hpp"
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

// The page's own numbers (Srgb, Monitor, LegacyMac show them; the gamut warning checks them): what the screen's sRGB
// came from — an RGB page's profile (an RGB ICC), a grey page's (a grey ICC), a CMYK page's (its inks: the gamut check
// alone reads them), or none (sRGB: the picture is its numbers).
struct Source {
    Source() = default;
    Source(core::Bytes profile, bool grey, bool inks = false, std::shared_ptr<const std::vector<float>> own = nullptr)
        : icc(std::move(profile)), gray(grey), cmyk(inks), values(std::move(own)) {}

    core::Bytes icc;
    bool gray = false;
    bool cmyk = false;
    // The page's values where render_page gives them (an RGB page of a profile: RenderResult::precise; a grey or CMYK
    // page: its inks), pixel by pixel as the picture's: channels() a pixel, 0..1 (CMYK: ink). None: worked back from the
    // picture (through the profile: relative, black point compensated, as the page was shown).
    std::shared_ptr<const std::vector<float>> values;

    unsigned channels() const { return cmyk ? 4u : gray ? 1u : 3u; }
    bool operator==(const Source& other) const;  // (its profile and kind)
};

// A Source's values from what render_page gives: a GKCR (RenderResult::precise: the colours, the alpha left out) or a
// grey or CMYK page's inks (its colour channels).
std::shared_ptr<const std::vector<float>> values_of(std::string_view gkcr);
std::shared_ptr<const std::vector<float>> values_of(const core::InkPixels& inks);

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
// (CIE ΔE 1976). The colours are the page's own (upstream's composite_profile: an RGB page's of its profile, a grey or
// CMYK page's inks), or the screen's sRGB. The ΔE is tabulated over them — 33³ points for RGB, 256 grey, 17⁴ CMYK — and
// read between its points as upstream's Clut does (tetrahedra; CMYK: two of them, along the cyan).
inline constexpr double kDefaultThreshold = 4.0;

class Gamut {
public:
    // The check against the setup's proof profile (its ICC; none: the export profile, or the plain conversion's round
    // trip) of the source's colours (its profile; none: sRGB. The plain conversion checks sRGB alone). Made once for each
    // profile and source (kept; the export profile read once for its file as it is: path, size, time).
    // core::Error("value") when a profile cannot be used.
    static std::shared_ptr<const Gamut> of(const Setup& setup, const Source& source = {});

    // How many values a colour it checks has (3: RGB, 1: grey, 4: CMYK), and whether they are the source's (false: the
    // screen's sRGB, the picture's).
    unsigned inputs() const { return inputs_; }
    bool of_source() const { return of_source_; }
    // ΔE76 between an sRGB colour (0..255) and its round trip (a check of sRGB's colours).
    float delta_e(int r, int g, int b) const;
    // ... between a colour (inputs() values, 0..1; CMYK: ink) and its round trip.
    float delta_e(const float* values) const;
    // Each pixel of the picture ("RGB" or "RGBA"; a transparent pixel never is): 255 out of gamut, else 0 — one byte a
    // pixel, row by row — and how many are. (A check of sRGB's colours.)
    std::vector<unsigned char> mask(const Image& image, double threshold, std::size_t* out = nullptr) const;
    // The same of the source's values (inputs() a pixel); `image` (the picture they are shown as) tells the transparent ones.
    std::vector<unsigned char> mask(const std::vector<float>& values, const Image& image, double threshold, std::size_t* out = nullptr) const;

    static constexpr int kGrid = 33;

private:
    unsigned inputs_ = 3;
    bool of_source_ = false;
    int points_ = kGrid;             // (along each input)
    std::vector<std::size_t> stride_;  // (the first input slowest, as an ICC table)
    std::vector<float> grid_;
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

// プロファイルの情報 (PC2d, edit.profileInfo: SPEC COLOR-04; upstream engine/src/color_cmds.rs:496-506 and 746-762,
// MIT): what an ICC profile says of itself — {"description", "space" (rgb, gray, cmyk, lab), "class" (display, output,
// input, colorspace, abstract, link, named), "version" ("4.3"), "white" ([X, Y, Z]: its media white point), "trc" (an
// RGB or grey profile's tone curve: "gamma 2.2", "parametric (type 4)", "table (N points)", "linear"; none for others),
// "lut" (whether it carries tables), "intents" (the rendering intents it holds), "bytes" (its size)}. No bytes: Little
// CMS's own sRGB, or (`lab`) its D50 Lab. core::Error("value") when Little CMS cannot read it.
core::Json profile_info(const core::Bytes& icc, bool lab = false);
// The built-in profiles: [{"id", "description", "space"}], PhotoCraft's order.
core::Json builtin_profile_list();
// The profile `spec` names in the book — a built-in's id (or alias, or its sha256:…), a page's number (its mode's
// profile: an RGB page's, sRGB for none; a CMYK or grey page's; a Lab page's D50 Lab), "sha256:…" of one the book
// holds — as profile_info says it, with "builtins" (builtin_profile_list) and, for a page, "page": its number.
// core::Error("key") when it names none.
core::Json profile_info_of(const core::Document& doc, std::string_view spec);
// The colour (sRGB, 0..255) in the proof's own values (情報パネル, PC2d): {"space": "cmyk"|"rgb"|"gray", "values": 0..1
// each (CMYK: ink)} — through the setup's proof profile (its intent and black point compensation), or with none
// through Genko's own CMYK for print (render/colour::to_cmyk with the export profile). core::Error("value") when the
// profile cannot be used.
core::Json proof_values(const Setup& setup, int r, int g, int b);

}  // namespace genko::render::proof
