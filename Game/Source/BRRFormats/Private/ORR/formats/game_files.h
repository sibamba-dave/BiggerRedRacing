// The one way into the original game's directory (code review A7, 2026-09-29).
//
// Every path under the game directory is built by game_path(), and every archive of it is
// opened through game_archive(), which unpacks it ONCE per process and shares the result.
// Before this, ALWAYS.WAP was decompressed by five different loaders and every race unpacked
// INGAME.WAP and ALWAYS.WAP again.
//
// The cache is process-wide, keyed by full path, rather than an object held by the App: the
// loaders that need it are free functions and the benches have no App, so an object would
// have had to be threaded through thirty signatures to buy nothing.
#pragma once

#include "formats/bigred_archive.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace bigred {

using Archive = std::unordered_map<std::string, Blob>;
using ArchivePtr = std::shared_ptr<const Archive>;

// The last place find_game_dir() looks: CMake's OPENBIGRED_GAME_DIR, empty unless set.
extern const char* const kDefaultGameDir;

// Where the game's own files are when no directory is named: the first folder holding
// ALWAYS.WAP among the executable's folder and up to three of its parents, the working
// directory and its parents, a `BIGRED` subfolder of each of those, `C:\BIGRED` (the original
// installer's default) and finally kDefaultGameDir, when the build set one. Empty when none does; `tried`, when given,
// receives every folder looked at, so the failure can say where it looked.
std::string find_game_dir(std::string* tried = nullptr);

// `game_dir` + separator + `file`. The only place the separator is written.
std::string game_path(const std::string& game_dir, const std::string& file);

// The archive `file` of the game directory (`ALWAYS.WAP`, `CHARSCN.SCN`, `DIRT1.LEV`...),
// unpacked on first use and shared afterwards: never null. Throws std::runtime_error when the
// file is missing or corrupt, like unpack_file - a failure is not cached, the next call tries
// again. Thread-safe.
//
// Everything is kept for the life of the process (all the .WAP, .SCN, .ENV and .VHC together
// unpack to about 8 MB) EXCEPT the levels, 22 MB for the 26 of them: only the last two
// `.LEV` are kept, which covers a race reading its own level from three places. So
// `const Archive& a = *game_archive(...)` is safe for anything but a level; for a `.LEV`, hold
// the pointer for as long as the archive is read.
ArchivePtr game_archive(const std::string& game_dir, const std::string& file);

// Declares an archive that is NOT on disk - a circuit made in memory (game/level_catalog.h's
// add_level declares its files as `<NAME>.LEV`). From then on game_archive() answers it for that
// name before looking at the disk, and keeps it for the life of the process; one of the same name
// is replaced. So every reader of a `.LEV` - the race, the presentation screen's picture, the
// menus' mini-map - takes an added circuit without knowing it is one.
void add_game_archive(const std::string& game_dir, const std::string& file, ArchivePtr archive);

// The same, or an empty archive when it cannot be opened - for the loaders to which a missing
// file only means "do without" (a race without a board, a menu without its cursor).
ArchivePtr game_archive_or_empty(const std::string& game_dir, const std::string& file);

// A single member of an archive, or nullptr when it is not there.
const Blob* find_member(const Archive& archive, const std::string& name);

} // namespace bigred
