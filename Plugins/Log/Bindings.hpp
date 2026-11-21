#pragma once

#include "nwnx.hpp"

#include <string_view>

// Fields bound to the running script run (pino child() bindings) and module-wide global bindings.
// Main thread only, like the scripts that set them.
namespace ScriptLog::Bindings {

// Merges an object into the current run's bindings. ExecuteScript()ed runs inherit them.
void Bind(const json& fields);
// Removes one key from the current run's bindings, or all of them for "".
void Unbind(std::string_view key);
// Replaces the global bindings; a non-object clears them.
void SetGlobal(const json& fields);
// The merged view a record made now gets: global, then each enclosing run, inner keys winning.
json Get();
// The innermost bound "logger" string, or nullptr.
const std::string* FindLogger();
// Adds the bindings a record made now gets, global first, then each enclosing run, outermost first, so an
// inner key wins. "logger" is left out: the record carries it as its origin.
void AddTo(NWNXLib::Log::Record& record);
// Adds one field: strings as strings (game-locale bytes in text, encoded in JSON), anything else as JSON.
void AddField(NWNXLib::Log::Record& record, std::string_view key, const json& value);

}
