#pragma once

#include <filesystem>
#include <map>
#include <string>

#include "core/json.hpp"

// The HTTP tokens (Python's genko/tokens.py): tokens.json in the person's config folder maps token → actor
// ({token: {"actor": "human:<name>" | "ai:<name>"}}, indent 2, owner-only). Tokens never go into a book.

namespace genko::api::tokens {

std::filesystem::path file();
// tokens.load: {token: actor} (empty without a file). core::Error("json") for a file that is not JSON (Python's words),
// core::Error("format") for an entry without an actor (Python stops with KeyError there).
std::map<std::string, std::string> load();
// tokens.add: a new token (32 random bytes, URL-safe base64) for the actor. core::PyValueError("actor must be
// human:<name> or ai:<name>") for any other actor.
std::string add(const std::string& actor);
// tokens.listing: [{"id": the token's first 8 characters, "actor"}].
core::Json listing();
// tokens.revoke: the tokens that start with `prefix` removed; how many.
int revoke(const std::string& prefix);

}  // namespace genko::api::tokens
