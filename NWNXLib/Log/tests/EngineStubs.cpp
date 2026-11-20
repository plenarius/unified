// The few engine and platform symbols the logger references, so the tests run without nwserver.
#include "nwnx.hpp"

bool g_bTracyEnabled = false;
bool g_bTracyProfilerConnected = false;
uint32_t g_bTracyScopeDisableCounter = 0;

namespace NWNXLib::Platform {

bool IsDebuggerPresent()
{
    return false;
}

std::string GetStackTrace(uint8_t)
{
    return "";
}

}
