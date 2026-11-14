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
// A face is installed (present) when its file is the file the catalogue lists: a regular file of that size whose
// SHA-256 is the catalogue's. That is found out once a process, the first time the face is asked for — its file is read
// whole then (at most 10 MB) and its bytes kept for the drawing that asked —, and the answer stays for the rest of the run:
// what is drawn (and remembered) of a face does not change while Genko runs. A face that is not installed is drawn in
// its kind's default face (render::text::face_of) and said (formats/checks: font_not_installed).
//
// Should a file that was good be gone or another file when its bytes are read again (they are let go when many faces
// are in use; the program upgraded while it runs), the face is not installed from then on: generation() moves, so what was
// drawn with it is drawn again in the default face, and the one piece of lettering being set at that moment takes the
// bundled Gothic for it (render::text::Fonts) — never another font.
//
// What may be called when:
//   - file_there(key), files_not_there(): one stat a face, nothing read — for a list on the screen (a picker's
//     "no file" mark). They say only that a file of the right size is there, not that it is the right file;
//   - present(key): reads that one face's file the first time (a few ms to some tens) — for drawing, checks and anything
//     that acts on one face a book names;
//   - missing(), verify(true): read all forty (about 160 MB) — a doctor's command, a package's test; never on the GUI
//     thread and never at start-up.
//
// The bytes of the files read are shared by everything that draws with them and kept, the last used first, up to
// kCacheBytes: a file is read whole and closed (never mapped: an open mapping would keep the file locked on Windows
// while Genko runs). Lettering in use holds its fonts' bytes itself, past that bound.

namespace genko::render::font_files {

inline constexpr std::uint64_t kCacheBytes = 64ULL * 1024 * 1024;

// The folder the files are looked for in (UTF-8; empty before there is a QCoreApplication to ask where the program is).
// Found out once and kept: prime() asks on the thread that made the application, at start-up, so that no drawing thread
// is the first to ask Qt.
std::string dir();
void prime();
// Tests: the folder to look in instead (nothing: the program's own rule again). What was learnt of the files and the
// bytes kept are forgotten; a caller that drew with them forgets its drawings too (render::text::clear_layout_cache).
void set_dir_for_testing(const std::optional<std::string>& dir);

// The file an added face is read from (UTF-8), there or not; empty for a key that is not one of the forty.
std::string path(std::string_view key);
// Whether a file of the catalogue's size is where an added face's file should be (one stat; and not found wrong before).
bool file_there(std::string_view key);
// The added faces with no such file, in the catalogue's order (forty stats).
std::vector<std::string> files_not_there();
// Whether an added face is installed: its file is the catalogue's (size and SHA-256). The first call for a face reads its file.
bool present(std::string_view key);
// An added face's file, read whole: shared, and kept among the last used. NotYetPorted("default_font") — what a font
// file that cannot be read is (render::TrueTypeFonts::truetype) — for a key that is not an added face or a face that is
// not installed; a file found gone or changed here makes the face not installed for the rest of the run.
std::shared_ptr<const QByteArray> bytes(std::string_view key);

// The added faces that are not installed, in the catalogue's order (reads every file not read yet).
std::vector<std::string> missing();
// How many times a face that was installed has been found gone or changed (and set_dir_for_testing called): what is
// remembered of drawings made before is not used after it moves.
std::uint64_t generation();

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
