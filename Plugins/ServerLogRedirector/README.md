@addtogroup serverlogredirector ServerLogRedirector
@page serverlogredirector Readme
@ingroup serverlogredirector 

Redirects server log output to the NWNX logger.

In text mode, it logs each line of `nwserverLog1.txt` (script prints, `WriteTimestampedLogEntry`, server events) as `(Server) <line>` at info level.

## JSON

With `NWNX_CORE_LOG_FORMAT=json`, every line the engine logs becomes one record instead: both `nwserverLog1.txt` and `nwengineLog.txt`, so script errors, SQLite errors, network and NWSync lines too.

```json
{"time":1763680000000,"level":50,"lvl":"error","msg":"Script error: my_script OID: 8000012a, Tag: GUARD, ERROR: TOO MANY INSTRUCTIONS","src":"engine","plugin":"NWNX_ServerLogRedirector","file":"nwsvmachinecommands.cpp","line":264,"cat":"ScriptVM","script":"my_script","oid":"8000012a","tag":"GUARD","area":"TOWN","func":"ReportError","err":{"message":"TOO MANY INSTRUCTIONS","code":632}}
```

- `level` from the engine's severity (debug, info, warn, error); `cat` is the engine's category: `User::NWScript`, `User::Chat`, `User::Info`, `Engine`, `ScriptVM`, `Network`, `NUI`, `ASSERT`, `NWNX::Core`, `NWNX::User`, or `General` for the lines the engine prints without one.
- `file`, `line` and `func` are the engine's source location; `msg` is the line without its trailing newline, and a message with several lines stays one record.
- During a script: the script context (`NWNX_CORE_LOG_CONTEXT`), and `fn` for `PrintString`, `PrintInteger`, `PrintFloat`, `PrintObject`, `PrintVector` and `WriteTimestampedLogEntry`.
- Script errors are at error level with `err.code` and `err.message`; SQLite errors add `err` and the `query` when the engine has it.
- The engine's own files keep exactly the same content. Records the engine makes on other threads in the `User::` categories, which it discards, are kept.
- `NWNX_SERVERLOGREDIRECTOR_LOG_LEVEL` filters records as for any plugin (7 includes the engine's debug lines).

| Variable Name | Value | Default | Notes |
|---|:-:|:-:|---|
| `NWNX_SERVERLOGREDIRECTOR_DROP_CATEGORIES` | string | | A comma-separated list of categories left out of the records, e.g. `Network,NUI`. The engine's files are unaffected. |
| `NWNX_SERVERLOGREDIRECTOR_ENGINE_FILES` | true/false | true | `false` stops the engine writing `nwserverLog1.txt` and `nwengineLog.txt` (and their `NWN_ENABLE_DEBUG_SPEW` copy on `stderr`), which only repeat the records. JSON only. |
