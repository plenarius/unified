#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <sys/types.h>

// File rotation shared by NWNX's own log file and the engine's: archives are <path>.YYYYmmddTHHMMSSZ, plus -N
// for a second one in the same second. Nothing here logs, so it can run under the log sink's lock.
namespace NWNXLib::Log::Rotation {

// A free archive name for path at the given time.
std::string ArchiveName(const std::string& path, std::time_t when);

// Deletes the oldest archives of path beyond keep; 0 keeps all.
void Prune(const std::string& path, int keep);

// Moves last run's file aside (named by its mtime) and prunes. Returns the archive, or "" if there was none.
std::string KeepPrevious(const std::string& path, int keep);

// "<bytes>[K|M|G]"; "0" is off. nullopt if malformed.
std::optional<off_t> ParseSize(const std::string& value);

// "hourly" or "daily" (UTC boundaries) in seconds; "off" is 0. nullopt if malformed.
std::optional<std::time_t> ParsePeriod(const std::string& value);

}
