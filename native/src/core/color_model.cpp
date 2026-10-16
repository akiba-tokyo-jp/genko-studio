#include "core/color_model.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <set>

#include "core/error.hpp"
#include "core/model.hpp"
#include "core/pyconv.hpp"

namespace genko::core {

namespace detail {
// (builtin_profiles_data.cpp, made by CMake from data/profiles/)
extern const unsigned char kProfile_display_p3[];
extern const std::size_t kProfile_display_p3_size;
extern const unsigned char kProfile_adobe_rgb_compat[];
extern const std::size_t kProfile_adobe_rgb_compat_size;
extern const unsigned char kProfile_prophoto_compat[];
extern const std::size_t kProfile_prophoto_compat_size;
extern const unsigned char kProfile_linear_srgb[];
extern const std::size_t kProfile_linear_srgb_size;
extern const unsigned char kProfile_rec2020[];
extern const std::size_t kProfile_rec2020_size;
extern const unsigned char kProfile_gray_gamma_2_2[];
extern const std::size_t kProfile_gray_gamma_2_2_size;
extern const unsigned char kProfile_sgray[];
extern const std::size_t kProfile_sgray_size;
extern const unsigned char kProfile_coated_cmyk[];
extern const std::size_t kProfile_coated_cmyk_size;
}  // namespace detail

namespace {

Bytes held(const unsigned char* data, std::size_t size) {
    return std::make_shared<const std::string>(reinterpret_cast<const char*>(data), size);
}

// PhotoCraft's Builtin::from_id: lower case, letters and digits only.
std::string folded(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

}  // namespace

std::string_view to_string(ProfileSpace space) {
    switch (space) {
        case ProfileSpace::Rgb: return "rgb";
        case ProfileSpace::Gray: return "gray";
        case ProfileSpace::Cmyk: return "cmyk";
        case ProfileSpace::Lab: return "lab";
    }
    return "rgb";
}

const std::vector<BuiltinProfile>& builtin_profiles() {
    // (the sha256 of each file in data/profiles/: test_color_model checks the bytes against them)
    static const std::vector<BuiltinProfile> table{
        {"srgb", "sRGB IEC61966-2.1", ProfileSpace::Rgb, "", nullptr},
        {"display-p3", "Display P3", ProfileSpace::Rgb, "354bc401f5df673a19a88edc1e3146145dbb278d3e3ca9062e400f68954414dc",
         held(detail::kProfile_display_p3, detail::kProfile_display_p3_size)},
        {"adobe-rgb-compat", "Adobe RGB (1998)-compatible (Photocraft)", ProfileSpace::Rgb,
         "b0a02be9fd60b859d2fcefda9460589005ffee222479635e8fb9b768f25d5fa1",
         held(detail::kProfile_adobe_rgb_compat, detail::kProfile_adobe_rgb_compat_size)},
        {"prophoto-compat", "ProPhoto RGB-compatible (ROMM primaries, Photocraft)", ProfileSpace::Rgb,
         "ada9d9fd72d1f3684b75716c85e1f53dd20c5aeb12895c7dfeded986cb1afc7a",
         held(detail::kProfile_prophoto_compat, detail::kProfile_prophoto_compat_size)},
        {"linear-srgb", "Linear sRGB (Photocraft)", ProfileSpace::Rgb, "1edb3a21bc500a43eb7761ba9342e7d19d5d4cb3cc8e682599463da67fbc73d9",
         held(detail::kProfile_linear_srgb, detail::kProfile_linear_srgb_size)},
        {"rec2020", "Rec. ITU-R BT.2020 (Photocraft)", ProfileSpace::Rgb, "0c848ef7e623414254b49addeae705dfb19a54a1717c241b8c32b6d952e23164",
         held(detail::kProfile_rec2020, detail::kProfile_rec2020_size)},
        {"gray-gamma-2.2", "Gray Gamma 2.2 (Photocraft)", ProfileSpace::Gray, "62389e1c7f7a354075cdef6ebd66324b1b25b28154cf30a18082ad93914dc04b",
         held(detail::kProfile_gray_gamma_2_2, detail::kProfile_gray_gamma_2_2_size)},
        {"sgray", "sGray (sRGB tone curve, Photocraft)", ProfileSpace::Gray, "52eea84d014a3b408cb50ac04a3f6f1a58eab0b2a721f4233635b3ad654d7128",
         held(detail::kProfile_sgray, detail::kProfile_sgray_size)},
        {"lab-d50", "Lab D50 identity (Photocraft)", ProfileSpace::Lab, "", nullptr},
        {"coated-cmyk", "Photocraft Coated CMYK (synthetic, 300% TAC, medium GCR)", ProfileSpace::Cmyk,
         "f7ba932085778fac4f8fed66e0a048351b44bb827eb07c1360b570bd3bfa54cc",
         held(detail::kProfile_coated_cmyk, detail::kProfile_coated_cmyk_size)},
    };
    return table;
}

const BuiltinProfile* builtin_profile(std::string_view name) {
    const std::string n = folded(name);
    // (PhotoCraft's aliases, crates/cms/src/builtin.rs:84-96)
    static const std::vector<std::pair<std::string_view, std::vector<std::string_view>>> kAliases{
        {"srgb", {"srgb", "srgbiec6196621", "srgbiec61966"}},
        {"display-p3", {"p3", "displayp3"}},
        {"adobe-rgb-compat", {"adobergb", "adobergb1998", "adobergbcompat", "clayrgb"}},
        {"prophoto-compat", {"prophoto", "prophotorgb", "prophotocompat", "romm", "rommrgb"}},
        {"linear-srgb", {"linearsrgb", "srgblinear", "linear"}},
        {"rec2020", {"rec2020", "bt2020"}},
        {"gray-gamma-2.2", {"gray", "graygamma22", "gamma22"}},
        {"sgray", {"sgray", "graysrgb"}},
        {"lab-d50", {"lab", "labd50", "cielab"}},
        {"coated-cmyk", {"cmyk", "coatedcmyk", "defaultcmyk", "photocraftcoatedcmyk"}},
    };
    const auto& table = builtin_profiles();
    const auto by_id = [&](std::string_view id) -> const BuiltinProfile* {
        for (const BuiltinProfile& p : table) {
            if (p.id == id) return &p;
        }
        return nullptr;
    };
    for (const auto& [id, names] : kAliases) {
        if (std::find(names.begin(), names.end(), n) != names.end()) return by_id(id);
    }
    for (const BuiltinProfile& p : table) {
        if (p.id == name) return &p;
        if (p.description.size() == name.size() &&
            std::equal(name.begin(), name.end(), p.description.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
            return &p;
        }
    }
    return nullptr;
}

const BuiltinProfile* builtin_profile_by_ref(std::string_view ref) {
    if (ref.size() != 71 || ref.substr(0, 7) != "sha256:") return nullptr;
    for (const BuiltinProfile& p : builtin_profiles()) {
        if (!p.sha256.empty() && ref.substr(7) == p.sha256) return &p;
    }
    return nullptr;
}

namespace {

[[noreturn]] void bad(const std::string& why) { throw Error("value", "the page's colour mode cannot be read: " + why); }

bool is_ref(std::string_view text) {
    if (text.size() != 71 || text.substr(0, 7) != "sha256:") return false;
    return std::all_of(text.begin() + 7, text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

RgbMode rgb_mode_from_json(const Json& value) {
    static const std::set<std::string> kKeys{"mode", "depth", "profile", "intent", "bpc"};
    for (const auto& [key, v] : value.items()) {
        if (!kKeys.contains(key)) bad("unknown key " + py_repr_str(key));
    }
    RgbMode out;
    const auto depth = value.find("depth");
    if (depth == value.end() || !depth->is_number_integer() ||
        (depth->get<std::int64_t>() != 8 && depth->get<std::int64_t>() != 16 && depth->get<std::int64_t>() != 32)) {
        bad("depth is 8, 16 or 32");
    }
    out.depth = static_cast<int>(depth->get<std::int64_t>());
    if (const auto profile = value.find("profile"); profile != value.end()) {
        if (!profile->is_string() || !is_ref(profile->get<std::string>())) bad("an RGB page's profile is sha256:<64 hex>");
        out.profile = profile->get<std::string>();
    }
    const auto intent = value.find("intent");
    if (intent == value.end() || !intent->is_string() || !is_intent(intent->get<std::string>())) {
        bad("intent is perceptual, relative, saturation or absolute");
    }
    out.intent = intent->get<std::string>();
    if (const auto bpc = value.find("bpc"); bpc != value.end()) {
        if (!bpc->is_boolean() || bpc->get<bool>()) bad("bpc is written only as false");
        out.bpc = false;
    }
    return out;
}

}  // namespace

Json to_json(const RgbMode& mode) {
    Json out = Json::object();
    out["mode"] = "rgb";
    out["depth"] = mode.depth;
    if (!mode.profile.empty()) out["profile"] = mode.profile;
    out["intent"] = mode.intent;
    if (!mode.bpc) out["bpc"] = false;
    return out;
}

ColorModel color_model_from_json(const Json& value) {
    ColorModel out;
    if (value.is_null()) return out;
    if (!value.is_object()) bad("it must be an object");
    const auto mode = value.find("mode");
    if (mode != value.end() && mode->is_string() && mode->get<std::string>() == "rgb") {
        RgbMode rgb = rgb_mode_from_json(value);
        if (!rgb.is_default()) out.rgb = std::move(rgb);
        return out;
    }
    out.inks = page_color_from_json(value);
    return out;
}

std::string color_raster_space(const std::optional<RgbMode>& rgb) { return rgb && !rgb->profile.empty() ? rgb->profile : std::string("srgb"); }

bool uses_color_model(const Document& doc) {
    const auto model = [](const std::optional<PageColor>& inks, const std::optional<RgbMode>& rgb) {
        return (inks && !inks->color_mode_only()) || rgb.has_value();
    };
    if (model(doc.color, doc.rgb)) return true;
    for (const auto& page : doc.pages) {
        if (model(page->color, page->rgb)) return true;
        for (const Layer& layer : page->layers) {
            if (layer.ink_raster && ink_raster_header(*layer.ink_raster).version != 1) return true;
        }
    }
    return false;
}

void validate_color_model(const Document& doc) {
    const auto check = [](const RgbMode& rgb, const std::string& where) {
        if (rgb.depth != 8 && rgb.depth != 16 && rgb.depth != 32) throw Error("value", where + ": an RGB page's depth is 8, 16 or 32");
        if (!rgb.profile.empty() && !is_ref(rgb.profile)) throw Error("value", where + ": an RGB page's profile is sha256:<64 hex>");
        if (!is_intent(rgb.intent)) throw Error("value", where + ": an unknown intent");
        if (rgb.icc && bytes_ref(*rgb.icc) != rgb.profile) throw Error("value", where + ": the ICC profile held is not " + rgb.profile);
    };
    if (doc.rgb) check(*doc.rgb, "the book");
    for (const auto& page : doc.pages) {
        if (std::find(doc.deferred.begin(), doc.deferred.end(), page) != doc.deferred.end()) continue;
        if (page->rgb) check(*page->rgb, "page " + page->index.repr());
    }
}

}  // namespace genko::core
