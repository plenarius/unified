#pragma once
#include "nwn_api.hpp"


#ifdef NWN_API_PROLOGUE
NWN_API_PROLOGUE(ExoLog)
#endif

enum LogSeverity
{
    LogSeverity_Debug   = 0,
    LogSeverity_Info    = 1,
    LogSeverity_Warning = 2,
    LogSeverity_Error   = 3,
};

enum LogCategory
{
    LogCategory_NWNXCore      = 0,
    LogCategory_NWNXUser      = 1,
    LogCategory_UserNWScript  = 2,
    LogCategory_UserChat      = 3,
    LogCategory_UserInfo      = 4,
    LogCategory_Assert        = 5,
    LogCategory_Engine        = 6,
    LogCategory_General       = 7,
    LogCategory_Network       = 8,
    LogCategory_ScriptVM      = 9,
    LogCategory_NUI           = 10,
};

namespace ExoLog
{
    // The engine's log: nwserverLog1.txt (User:: categories and Engine), nwengineLog.txt (the rest), and
    // stderr with NWN_ENABLE_DEBUG_SPEW. Messages with newlines are split into one line per part.
    void Emit(LogSeverity severity, LogCategory category, const char* func, const char* file, int line, const char* format, ...);
}


#ifdef NWN_API_EPILOGUE
NWN_API_EPILOGUE(ExoLog)
#endif
