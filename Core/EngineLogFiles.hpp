#pragma once

#include <optional>
#include <string>

// The engine's own log files: nwengineLog.txt and nwserverLog<N>.txt.
namespace Core::EngineLogFiles {

enum class File { Engine, Server };

// Moves the live file to <name>.<UTC time> and lets the engine continue in a new file
// under the same name. Main thread only. Returns the archive path, or nullopt (logged).
std::optional<std::string> Rotate(File file);

// Runs the size and time rotation triggers. Called from the main loop; returns at once when none are set.
void Tick();

}
