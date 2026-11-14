#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class QByteArray;

// fonts@1 (書体の追加 F2): the files of the forty added lettering faces (core/font_catalog.hpp). They are not compiled into
// the program: a package installs them in share/genko-studio/fonts beside bin/, and the program looks in exactly one
// place — <the folder the program is in>/../share/genko-studio/fonts — whatever the working folder or the environment is
// (no environment variable, no search). A build tree has that folder beside its program too (native/src/api/CMakeLists.txt
// links it to native/resources/fonts). Tests name the folder themselves (set_dir_for_testing).
//
// A face is "there" when its file is a regular file of the size the catalogue says — asked of the disk once a process, so
// what is drawn (and remembered) of a face does not change while Genko runs. A face whose file is not there is drawn in
// its kind's default face (render::text::face_of) and listed (missing()).
//
// The bytes of the files read are shared by everything that draws with them and kept, the last used first, up to
// kCacheBytes: a file is read whole and closed (never mapped: an open mapping would keep the file locked on Windows
// while Genko runs).

namespace genko::render::font_files {

inline constexpr std::uint64_t kCacheBytes = 64ULL * 1024 * 1024;

// The folder the files are looked for in (UTF-8; empty before there is a QCoreApplication to ask where the program is).
std::string dir();
// Tests: the folder to look in instead (nothing: the program's own rule again). What was learnt of the files and the
// bytes kept are forgotten; a caller that drew with them forgets its drawings too (render::text::clear_layout_cache).
void set_dir_for_testing(const std::optional<std::string>& dir);

// The file an added face is read from (UTF-8), there or not; empty for a key that is not one of the forty.
std::string path(std::string_view key);
// Whether an added face's file is there.
bool present(std::string_view key);
// An added face's file, read whole: shared, and kept among the last used. NotYetPorted("default_font") — what a font
// file that cannot be read is (render::TrueTypeFonts::truetype) — for a key that is not an added face, a file that is
// not there, or one that is not the size it was.
std::shared_ptr<const QByteArray> bytes(std::string_view key);

// The added faces whose files are not there, in the catalogue's order.
std::vector<std::string> missing();

// Every added face's file looked at now (not the answers kept): whether it is a regular file, of the catalogue's size
// and — `hashes` — of its SHA-256 (reads all forty: about 160 MB).
struct Checked {
    std::string key;
    std::string file;  // its name in the folder
    bool exists = false;
    bool size_ok = false;
    std::optional<bool> hash_ok;  // (nothing: not asked, or not read)
};
std::vector<Checked> verify(bool hashes);

// The bytes kept now, and letting them go (those in use stay with their users).
std::uint64_t cached_bytes();
void clear_cache();

}  // namespace genko::render::font_files
