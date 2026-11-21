// Built into two plugin modules with different PLUGIN_NAMEs; see LogLevels.cpp.
#include "nwnx.hpp"

using namespace NWNXLib;

extern "C" void LogTest_Debug()
{
    LOG_DEBUG("first translation unit");
}

extern "C" void LogTest_SetLogLevel(const char* plugin, int level)
{
    Log::SetLogLevel(plugin, static_cast<Log::Channel::Enum>(level));
}
