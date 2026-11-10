#include "storage/state.hpp"

#include "storage/asset_store.hpp"

namespace genko::storage {

core::Json state_of(const core::Json& payload) {
    core::Json state = payload;
    if (state.is_object()) {
        state.erase("revision");
        state.erase("writer");
    }
    return state;
}

std::string state_text(const core::Json& payload) { return core::dump_canonical(state_of(payload)); }

std::string state_ref(const core::Json& payload) { return AssetStore::ref(state_text(payload)); }

}  // namespace genko::storage
