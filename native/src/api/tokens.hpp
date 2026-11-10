#pragma once

// (PC5 J-g) The tokens live in the service now (service/tokens.hpp: the app's control channel reads them too); the
// command line's servers name them as before.
#include "service/tokens.hpp"

namespace genko::api {
namespace tokens = service::tokens;
}  // namespace genko::api
