#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/json.hpp"
#include "core/model.hpp"

// Generation requests (Python's genko/studio/genreq.py, genko.genreq@1): everything an agent hands to its image tool.
// Genko generates nothing. It writes studio/requests/<id>/request.json with the suggested sizes, a prompt draft (ja, en,
// tags), what not to draw, where to keep calm for balloons, and files: guides (composition, pose, keepout), copies of the
// approved reference images, and for fixes the source image and a mask. The id is a hash of the content, so asking again
// for the same thing gives the same request — byte for byte as Python's (its guides drawn as Pillow draws them).

namespace genko::service::genreq {

using core::Json;

inline constexpr const char* kType = "genko.genreq@1";
inline constexpr double kPadMm = 3.0;
inline constexpr std::int64_t kTargetPixels = 1024 * 1024;

// genreq.RequestError (a ValueError): the message and where in the request it is.
class RequestError : public std::runtime_error {
public:
    RequestError(const std::string& message, std::string path = "/") : std::runtime_error(message), path_(std::move(path)) {}
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

struct Pack {
    Json request = Json::object();
    std::map<std::string, std::string> files;  // relative name → bytes
    std::string id() const { return request.at("id").get<std::string>(); }
};

// genreq.vocab(expression): the phrase book (vocab.json); colour pages get the colour style.
Json vocab(const std::string& expression = "mono");
// genreq.book_vocab(episode, expression): the phrase book with the book's catalog style when it fits the page.
Json book_vocab(const core::Document& doc, const std::string& expression = "mono");
// genreq.suggested_px(aspect, pixels)
Json suggested_px(double aspect, std::int64_t pixels = kTargetPixels);

struct Options {
    Json purpose = "panel_art";
    Json mode = "new";
    Json page;
    Json frame_id;
    Json character_id;
    Json location_id;
    Json parent;
    Json tool;
    Json instruction;
    Json regions;
    Json focus_character;
};

// genreq.build(episode, project, …): the request and its files. RequestError for what cannot be asked.
Pack build(const core::Document& doc, const std::filesystem::path& project, Options options);
// genreq.write(pack, project): studio/requests/<id>/ with request.json and its files, and the inbox folder (idempotent).
std::filesystem::path write(const Pack& pack, const std::filesystem::path& project);
// genreq.record(pack): the open_request op's body for this pack.
Json record(const Pack& pack);

}  // namespace genko::service::genreq
