@page log Readme
@ingroup log

Structured logging from NWScript into the NWNX log.

Each record carries:

1. the context of the running script: `script` (`!Chunk` for ExecuteScriptChunk), `oid`, `tag` and `area` of `OBJECT_SELF`, chosen with `NWNX_CORE_LOG_CONTEXT` (see the Core README; in JSON, NWNX's own records made during a script get it too, but no bindings);
2. the global bindings set with `NWNX_Log_SetGlobal()`;
3. the bindings of the current script run and of every script that ran it with `ExecuteScript()`, set with `NWNX_Log_Bind()` and `NWNX_Log_SetLogger()`;
4. the fields passed to the call.

A later source wins when a key repeats. Bindings end with the script run that set them, also when it is aborted with `AbortRunningScript()` or by the instruction limit. They don't carry into `DelayCommand()` or `AssignCommand()`, which run later as scripts of their own.

```c
NWNX_Log_SetLogger("bank");
NWNX_Log_Bind(JsonParse("{\"txn\":\"a1b2\"}"));
NWNX_Log_Info("deposit", JsonParse("{\"gold\":250}"));
// I [12:00:01] [NWNX_Log] deposit logger=bank script=bank_deposit oid=8000012a tag=BANKER area=cityhall txn=a1b2 gold=250
// {"time":1763640001000,"level":30,"lvl":"info","msg":"deposit","src":"script","plugin":"NWNX_Log","logger":"bank","script":"bank_deposit","oid":"8000012a","tag":"BANKER","area":"cityhall","txn":"a1b2","gold":250}
```

In text mode the fields follow the message as `key=value`, strings in the game's encoding like the message; values with spaces, `=`, `"`, `\` or control characters are quoted. In JSON (`NWNX_CORE_LOG_FORMAT=json`) the record has `"src":"script"` and the `logger` after `plugin`; script records have no `file` or `line`.

`NWNX_Log_Rotate()` rotates NWNX's own log file, `nwserverLog1.txt` and `nwengineLog.txt` like the `logrotate` console command (see `NWNX_CORE_LOG_FILE_*` and `NWNX_CORE_LOG_ENGINE_*` in the Core README).

Guard expensive fields with `NWNX_Log_IsEnabled()`: a filtered call returns before it looks at its fields, but the script still builds them.

## Environment Variables

| Variable Name | Value | Default | Notes |
| ------------- | :---: | ------- | ----- |
| `NWNX_LOG_LOG_LEVEL` | int | `NWNX_CORE_LOG_LEVEL` | The level of script records, as for any plugin (7 = DEBUG, 3 = ERROR). |
| `NWNX_LOG_LOGGER_LEVELS` | string | Unset | Per-logger levels that override `NWNX_LOG_LOG_LEVEL`, e.g. `bank:7,craft:4`. |
