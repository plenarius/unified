// A second translation unit of the same plugin; see LogLevels.cpp.
#include "nwnx.hpp"

extern "C" void LogTest_Debug2()
{
    LOG_DEBUG("second translation unit");
}
