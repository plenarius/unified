/// @addtogroup log Log
/// @brief Structured logging from NWScript into the NWNX log.
///
/// Every record carries the running script's context (`script`, `oid`, `tag`, `area`), the bindings
/// set with NWNX_Log_Bind() and NWNX_Log_SetGlobal(), and the fields passed to the call.
/// @{
/// @file nwnx_log.nss

const string NWNX_Log = "NWNX_Log"; ///< @private

/// @name Log levels
/// @anchor log_levels
/// The same numbers as the NWNX_*_LOG_LEVEL environment variables. Scripts can't log at FATAL.
/// @{
const int NWNX_LOG_LEVEL_DEBUG   = 7;
const int NWNX_LOG_LEVEL_INFO    = 6;
const int NWNX_LOG_LEVEL_NOTICE  = 5;
const int NWNX_LOG_LEVEL_WARNING = 4;
const int NWNX_LOG_LEVEL_ERROR   = 3;
/// @}

/// @name Rotation targets
/// @anchor log_rotate
/// Flags for NWNX_Log_Rotate(); combine them with `|`.
/// @{
const int NWNX_LOG_ROTATE_NWNX   = 1; ///< NWNX's own log file: reserved, does nothing yet.
const int NWNX_LOG_ROTATE_SERVER = 2; ///< nwserverLog1.txt
const int NWNX_LOG_ROTATE_ENGINE = 4; ///< nwengineLog.txt
const int NWNX_LOG_ROTATE_ALL    = 7;
/// @}

/// @brief Logs a message at DEBUG.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
void NWNX_Log_Debug(string sMessage, json jFields = JSON_NULL);

/// @brief Logs a message at INFO.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
void NWNX_Log_Info(string sMessage, json jFields = JSON_NULL);

/// @brief Logs a message at NOTICE.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
void NWNX_Log_Notice(string sMessage, json jFields = JSON_NULL);

/// @brief Logs a message at WARNING.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
void NWNX_Log_Warning(string sMessage, json jFields = JSON_NULL);

/// @brief Logs a message at ERROR.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
void NWNX_Log_Error(string sMessage, json jFields = JSON_NULL);

/// @brief Logs a message at the given level.
/// @param nLevel A @ref log_levels "log level"; out-of-range levels are clamped to DEBUG or ERROR.
/// @param sMessage The message.
/// @param jFields A JSON object of fields to add to the record. Anything else is logged under `data`.
/// @param sLogger The logger (component) name for this record, overriding the bound one.
void NWNX_Log_Write(int nLevel, string sMessage, json jFields = JSON_NULL, string sLogger = "");

/// @brief Checks whether a record at nLevel would be logged.
/// @note Use it to skip building expensive fields: `if (NWNX_Log_IsEnabled(NWNX_LOG_LEVEL_DEBUG)) ...`
/// @param nLevel A @ref log_levels "log level".
/// @param sLogger The logger to check; "" checks the bound logger, if any.
/// @return TRUE if the record would be logged under NWNX_LOG_LOG_LEVEL and NWNX_LOG_LOGGER_LEVELS.
int NWNX_Log_IsEnabled(int nLevel, string sLogger = "");

/// @brief Binds fields to the current script run, like a pino child logger.
/// @note Every record made until the script ends carries them, including those of scripts it
/// runs with ExecuteScript(), which can override them with bindings of their own. They don't carry
/// into DelayCommand() or AssignCommand(), which run later as scripts of their own.
/// @param jFields A JSON object, merged into the run's bindings.
void NWNX_Log_Bind(json jFields);

/// @brief Removes a binding of the current script run.
/// @note Bindings of an enclosing script stay; override them with NWNX_Log_Bind() instead.
/// @param sKey The key to remove, or "" to remove all of the run's bindings.
void NWNX_Log_Unbind(string sKey = "");

/// @brief Gets the bindings a record made now would carry.
/// @return A JSON object of the global bindings and those of every enclosing script run, inner keys winning.
json NWNX_Log_GetBindings();

/// @brief Sets the logger (component) name for the current script run.
/// @note Shorthand for NWNX_Log_Bind() with `{"logger": sLogger}`. NWNX_LOG_LOGGER_LEVELS can set its own level.
/// @param sLogger The logger name, for example "bank".
void NWNX_Log_SetLogger(string sLogger);

/// @brief Sets module-wide bindings that every record carries until they are changed.
/// @param jFields A JSON object that replaces the previous global bindings; JSON_NULL clears them.
void NWNX_Log_SetGlobal(json jFields);

/// @brief Rotates log files as the `logrotate` console command does: the file moves to `<name>.<UTC time>`
/// and logging continues in a new one. NWNX_CORE_LOG_ENGINE_KEEP sets how many archives stay.
/// @param nTargets @ref log_rotate "Rotation targets".
/// @return TRUE if every requested engine and server log rotation ran.
int NWNX_Log_Rotate(int nTargets = NWNX_LOG_ROTATE_ALL);

/// @}

void NWNX_Log_Debug(string sMessage, json jFields = JSON_NULL)
{
    NWNX_Log_Write(NWNX_LOG_LEVEL_DEBUG, sMessage, jFields);
}

void NWNX_Log_Info(string sMessage, json jFields = JSON_NULL)
{
    NWNX_Log_Write(NWNX_LOG_LEVEL_INFO, sMessage, jFields);
}

void NWNX_Log_Notice(string sMessage, json jFields = JSON_NULL)
{
    NWNX_Log_Write(NWNX_LOG_LEVEL_NOTICE, sMessage, jFields);
}

void NWNX_Log_Warning(string sMessage, json jFields = JSON_NULL)
{
    NWNX_Log_Write(NWNX_LOG_LEVEL_WARNING, sMessage, jFields);
}

void NWNX_Log_Error(string sMessage, json jFields = JSON_NULL)
{
    NWNX_Log_Write(NWNX_LOG_LEVEL_ERROR, sMessage, jFields);
}

void NWNX_Log_Write(int nLevel, string sMessage, json jFields = JSON_NULL, string sLogger = "")
{
    NWNXPushString(sLogger);
    NWNXPushJson(jFields);
    NWNXPushString(sMessage);
    NWNXPushInt(nLevel);
    NWNXCall(NWNX_Log, "Write");
}

int NWNX_Log_IsEnabled(int nLevel, string sLogger = "")
{
    NWNXPushString(sLogger);
    NWNXPushInt(nLevel);
    NWNXCall(NWNX_Log, "IsEnabled");
    return NWNXPopInt();
}

void NWNX_Log_Bind(json jFields)
{
    NWNXPushJson(jFields);
    NWNXCall(NWNX_Log, "Bind");
}

void NWNX_Log_Unbind(string sKey = "")
{
    NWNXPushString(sKey);
    NWNXCall(NWNX_Log, "Unbind");
}

json NWNX_Log_GetBindings()
{
    NWNXCall(NWNX_Log, "GetBindings");
    return NWNXPopJson();
}

void NWNX_Log_SetLogger(string sLogger)
{
    NWNX_Log_Bind(JsonObjectSet(JsonObject(), "logger", JsonString(sLogger)));
}

void NWNX_Log_SetGlobal(json jFields)
{
    NWNXPushJson(jFields);
    NWNXCall(NWNX_Log, "SetGlobal");
}

int NWNX_Log_Rotate(int nTargets = NWNX_LOG_ROTATE_ALL)
{
    NWNXPushInt(nTargets);
    NWNXCall(NWNX_Log, "Rotate");
    return NWNXPopInt();
}
