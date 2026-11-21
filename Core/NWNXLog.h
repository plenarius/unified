/* NWNX structured log entry points for language bridges (DotNET/Anvil, SWIG, Ruby), exported from NWNX_Core.so.
 * Plain C: safe to call from any thread, all pointers may be NULL, nothing is thrown. Strings are UTF-8. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* syslogLevel: 7 debug, 6 info, 5 notice, 4 warning, 3 error; others are clamped (no fatal).
 * src: "dotnet", "swig" or "ruby"; it selects the NWNX_<PLUGIN>_LOG_LEVEL threshold and the plugin name.
 * logger: optional name; NWNX_LOG_LOGGER_LEVELS can give it its own threshold.
 * fieldsJson: a JSON object whose members become fields, or NULL; at most 64 levels deep. */
void NWNX_Log_Emit(int syslogLevel, const char* src, const char* logger, const char* msg, const char* fieldsJson);

/* Non-zero when NWNX_Log_Emit with the same arguments would write, so a caller can skip building the record. */
int NWNX_Log_IsEnabled(int syslogLevel, const char* src, const char* logger);

#ifdef __cplusplus
}
#endif
