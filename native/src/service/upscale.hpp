#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.hpp"
#include "render/image.hpp"

// The upscaler programs a person registered on this computer (Python's genko/upscale.py, the registry: <config>/
// upscalers.json, written only by a person with `genko studio upscaler`):
//   {"<name>": {"command": ["realesrgan-ncnn-vulkan", "-i", "{in}", "-o", "{out}", "-s", "{scale}"], "scales": [2, 4],
//               "label": "…"}}

namespace genko::service::upscale {

using core::Json;

inline constexpr const char* kBuiltin = "genko";
inline constexpr double kMaxScale = 4.0;
inline constexpr int kTimeoutSeconds = 900;

// upscale.registered(): the registered programs (an unreadable file or one that is not an object: none; entries
// without a command list left out).
Json registered();
// upscale.available(): the enlargers an agent can name, Genko's own first.
Json available();
// upscale.register(name, command, scales, label): the entry kept (core::PyValueError with Python's words for a name
// or a command it does not take; shlex.split's ValueError for a command line it cannot split).
Json register_program(const std::string& name, const Json& command, const std::vector<double>& scales, const std::string& label);
// upscale.unregister(name): whether it was there.
bool unregister(const std::string& name);

// upscale.builtin(image, scale): Lanczos enlargement, then an unsharp mask sized to the enlargement. Worked out in bands
// of rows on `threads` threads at most (0: render::band_threads()), with the same pixels whatever their number.
render::Image builtin(const render::Image& image, double scale, int threads = 0);
// upscale.external(name, image, scale): the person's upscaler run on the picture (its output resized to exactly `scale`).
// core::PyValueError with Python's words when it is not registered, cannot be started, fails or makes no picture.
render::Image external(const std::string& name, const render::Image& image, double scale);
// upscale.enlarge(image, scale, method): PyValueError "scale は 1〜4" for a scale out of range.
render::Image enlarge(const render::Image& image, double scale, const std::string& method = kBuiltin);

// shlex.split(text) (POSIX: quotes, backslashes; ValueError "No closing quotation" / "No escaped character").
std::vector<std::string> shlex_split(std::string_view text);

}  // namespace genko::service::upscale
