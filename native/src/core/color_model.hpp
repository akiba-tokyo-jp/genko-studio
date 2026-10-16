#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/ink.hpp"

// color-model@1 (docs/cpp-migration/schema-v4.md「色モデル」, SPEC.md COLOR-03): the colour model of a page past
// color-mode@1 — RGB pages of another profile or depth, grey pages, 32-bit pixels — and the ICC profiles the C++ build
// carries itself.

namespace genko::core {

// The colour space an ICC profile is for.
enum class ProfileSpace { Rgb, Gray, Cmyk, Lab };

std::string_view to_string(ProfileSpace space);

// A built-in profile (PhotoCraft v0.2.0's ids, aliases and descriptions: crates/cms/src/builtin.rs:18-102). srgb is
// Little CMS's own sRGB (a page with no profile) and lab-d50 Little CMS's D50 Lab v4: they carry no bytes. The others'
// bytes are built into the program (data/profiles/<id>.icc) and named by their sha256.
struct BuiltinProfile {
    std::string_view id;
    std::string_view description;
    ProfileSpace space = ProfileSpace::Rgb;
    std::string_view sha256;  // (of the bytes; "" for srgb and lab-d50)
    Bytes bytes;              // (null for srgb and lab-d50)

    // The bytes' asset ref ("sha256:…"), or "" for one with no bytes.
    std::string ref() const { return sha256.empty() ? std::string() : "sha256:" + std::string(sha256); }
};

// Every built-in profile, in PhotoCraft's order (srgb, display-p3, adobe-rgb-compat, prophoto-compat, linear-srgb,
// rec2020, gray-gamma-2.2, sgray, lab-d50, coated-cmyk).
const std::vector<BuiltinProfile>& builtin_profiles();
// A built-in profile by its id (any case; PhotoCraft's aliases, its description), or null.
const BuiltinProfile* builtin_profile(std::string_view name);
// The built-in profile whose bytes these are (by their asset ref), or null.
const BuiltinProfile* builtin_profile_by_ref(std::string_view ref);

}  // namespace genko::core
