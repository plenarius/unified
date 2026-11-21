// Stands in for a Core translation unit that logs, so the plugins below share a process with it.
#include "nwnx.hpp"

extern "C" void LogTest_CoreDebug()
{
    LOG_DEBUG("core");
}
