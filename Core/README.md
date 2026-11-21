@page nwnx Core Functionality
@ingroup nwnx

The core of NWNX:EE that does all the things.

## Environment Variables

| Variable Name | Value | Default | Notes |
| ------------- | :---: | ------- | ----- |
| `NWNX_CORE_SKIP` | 0-1 | 0 | Set whether to load NWNX or not.
| `NWNX_CORE_LOAD_PATH` | string | Unset | The directory from where plugins are loaded. Defaults to where NWNX_Core is located.
| `NWNX_CORE_SKIP_ALL` | 0-1 | 0 | Changes the default plugin load state from 'load all' to 'skip all'. Use `NWNX_{PLUGIN}_SKIP=n` to enable specific plugins in this case.
| `NWNX_CORE_LOG_LEVEL` | 0-7 | 5 | Sets the default log level. `6` is a good level for live servers. `7`  includes debug messages which can be helpful if you're having issues.
| `NWNX_CORE_LOAD_EXPERIMENTAL_PLUGIN` | 0-1 | 0 | Allows loading of NWNX_Experimental, use at your own risk.
| `NWNX_CORE_LOCALE` | string | Unset | Set the locale NWNX will use when encoding. Supported are cp1250, cp1251, cp1252 (default). Can also use two letter language codes (e.g. `"ru"`, `"cs"`)
| `NWNX_CORE_ENCODING_BEST_FIT` | 0-1 | 0 | When converting from UTF-8, replace characters the codepage lacks (typographic spaces, hyphens, reversed quotes, combining accents) with close equivalents instead of `?`.
| `NWNX_CORE_CRASH_ON_ASSERT_FAILURE` | 0-1 | 0 | Sets whether an ASSERT failure hard crashes the server.
| `NWNX_CORE_SHUTDOWN_SCRIPT` | string | Unset | Sets which NWScript to run when the module shuts down.
| `NWNX_CORE_ALLOW_NWNX_FUNCTIONS_IN_EXECUTE_SCRIPT_CHUNK` | 0-1 | 0 | When enabled, allows the ExecuteScriptChunk() function to call NWScript NWNX functions.
| `NWNX_CORE_NWNX_RESOURCE_DIRECTORY_PATH` | string | UserDirectory/nwnx | The path of the /nwnx resource directory
| `NWNX_CORE_CUSTOM_RESMAN_DEFINITION` | string | Unset | A path to a file with custom resource directory aliases, see below for more info
| `NWNX_CORE_NWNX_RESOURCE_DIRECTORY_PRIORITY` | int | 70000000 | Sets the resman priority of the UserDirectory/nwnx folder.
| `NWNX_CORE_LOG_TIMESTAMP` | 0-1 | 1 | Set whether to show timestamp in logs printed by NWNX.
| `NWNX_CORE_LOG_DATE` | 0-1 | 0 | Set whether to show date(Y-M-D) in logs printed by NWNX. Timestamps must be enabled.
| `NWNX_CORE_LOG_PLUGIN` | 0-1 | 1 | Set whether to show plugin name in logs printed by NWNX.
| `NWNX_CORE_LOG_SOURCE` | 0-1 | 1 | Set whether to show source code location in logs printed by NWNX.
| `NWNX_CORE_LOG_COLOR` | 0-1 | 1 | Set whether to show logs printed by NWNX in color (only when printing to a TTY).
| `NWNX_CORE_LOG_FORCE_COLOR` | 0-1| 0 | Sets whether to force color output.
| `NWNX_CORE_LOG_FILE_PATH` | string | Unset | Sets the secondary (in addition to `stdout`) log file.
| `NWNX_CORE_LOG_FORMAT` | text, json | text | The format of the logs printed to `stdout`. `json` prints one JSON object per line (see below); then only `LOG_SOURCE` of the options above applies.
| `NWNX_CORE_LOG_FILE_FORMAT` | text, json | `LOG_FORMAT` | The format of the log file.
| `NWNX_CORE_LOG_TEXT_FIELDS` | 0-1 | 0 | Appends the fields of structured records to text lines as ` key=value`, strings in game-locale bytes like the rest of the line (quoted when they hold a space, `=`, `"` or a control character), other values as JSON writes them. By default text lines print only the message.
| `NWNX_CORE_LOG_JSON_LEVEL` | number, label | number | `number`: `"level":30,"lvl":"info"`. `label`: `"level":"info"`.
| `NWNX_CORE_LOG_JSON_ENCODING` | auto, locale, utf8 | auto | How strings become UTF-8. `auto`: valid UTF-8 is kept, anything else is converted from `NWNX_CORE_LOCALE`. `locale`: always converted. `utf8`: invalid bytes become U+FFFD.
| `NWNX_CORE_LOG_JSON_STRIP_COLORS` | 0-1 | 1 | Removes NWN colour tokens (`<cXYZ>`, `</c>`) from JSON strings.
| `NWNX_CORE_LOG_JSON_MAX_VALUE` | int | 8192 | Longer strings are cut (at a character boundary) and the record gets `"truncated":true`.
| `NWNX_CORE_LOG_JSON_MAX_LINE` | int | 65536 | Longer records are shortened the same way: the message first, then the fields are dropped.
| `NWNX_CORE_LOG_JSON_STATIC` | JSON object | Unset | Added to every JSON record, e.g. `{"server":"main"}`.
| `NWNX_CORE_LOG_CONTEXT` | string | `script,oid,tag,area` | The keys of the running script added to records with fields (JSON, `LOG_TEXT_FIELDS`) made during a script: `script`, `oid` (`OBJECT_SELF`), its `tag` and `area` tag, and, only when listed, `pc` (character name) and `player` (login name) for player-controlled creatures. `none` turns it off.
| `NWNX_CORE_LOG_CAPTURE_STDIO` | auto, true, false | auto | With `LOG_FORMAT=json`, turn every other line on `stdout` and `stderr` (the engine's and libraries' `printf`s) into a JSON record `{"level":30,"src":"stdout","msg":...}` on the same stream, so every line is JSON. Lines that are already one JSON object pass through unchanged; ANSI colour codes and blank lines are dropped. `auto` and `true` turn it on with JSON `stdout`; `false` leaves the streams alone. Decided at startup, like `LOG_ASYNC`.
| `NWNX_CORE_LOG_ASYNC` | auto, true, false | auto | Write logs on a background thread, so a slow `stdout` or disk does not stall the server. `auto`: on when `LOG_FORMAT` or `LOG_FILE_FORMAT` is `json`. Decided at startup: switching to JSON later with `logformat` stays synchronous. The bytes are the same, but NWNX lines can reach `stdout` up to `LOG_ASYNC_FLUSH_MS` after other output.
| `NWNX_CORE_LOG_ASYNC_FLUSH_MS` | int | 50 | The longest a line waits before it is written. Warnings and worse, and every 64 KiB of lines, are written at once.
| `NWNX_CORE_LOG_ASYNC_MAX_BYTES` | int | 8388608 | How much may wait to be written (at least 262144).
| `NWNX_CORE_LOG_ASYNC_FULL` | block, drop | block | When that is full: `block` waits for room; `drop` drops debug, info and notice lines and later logs a warning with the number dropped. Warnings and worse always wait.
| `NWNX_CORE_LOG_FILE_KEEP_PREVIOUS` | true/false | false | Move the previous run's `LOG_FILE_PATH` to `<name>.<UTC time>` at startup instead of appending to it.
| `NWNX_CORE_LOG_FILE_KEEP` | int | 7 | The number of archives of `LOG_FILE_PATH` kept, by `LOG_FILE_KEEP_PREVIOUS` and by rotation. `0` keeps all.
| `NWNX_CORE_LOG_FILE_ROTATE_SIZE` | bytes, with an optional `K`, `M` or `G` | 0 | When above 0, rotate `LOG_FILE_PATH` (as `logrotate nwnx`) once this much has been written to it.
| `NWNX_CORE_LOG_FILE_ROTATE_TIME` | off, hourly, daily | off | Rotate `LOG_FILE_PATH` at every full hour or at midnight UTC, unless nothing was written since the last rotation.
| `NWNX_CORE_LOG_REOPEN_SIGNAL` | off, HUP, USR1, USR2 | off | On this signal, reopen the log files an external tool (e.g. logrotate without `copytruncate`) moved away, as `logreopen` does. `USR1` is the recommended choice; with `off` no handler is installed, so the signal keeps its default action.
| `NWNX_CORE_LOG_ENGINE_KEEP_PREVIOUS` | true/false | false | Keep the previous run's `nwengineLog.txt` and `nwserverLog1.txt` as `<name>.<UTC time>` instead of the engine overwriting them.
| `NWNX_CORE_LOG_ENGINE_KEEP` | int | 7 | The number of archives kept of each engine log file, by `NWNX_CORE_LOG_ENGINE_KEEP_PREVIOUS` and by rotation. `0` keeps all.
| `NWNX_CORE_LOG_ENGINE_ROTATE_SIZE` | bytes, with an optional `K`, `M` or `G` | 0 | When above 0, rotate an engine log file (as `logrotate`) once it reaches this size. Checked every 10 seconds.
| `NWNX_CORE_LOG_ENGINE_ROTATE_TIME` | off, hourly, daily | off | Rotate the engine log files at every full hour or at midnight UTC, unless nothing was written since the last rotation.
| `NWNX_CORE_HARD_EXIT` | 0-1| 0 | If set, NWNX will hard kill the process after it unloads.
| `NWNX_CORE_BASE_GAME_CRASH_HANDLER` | 0-1 | 0 | Sets whether to also call the base game handler in case of crash.

## Console Commands

| Command | Description |
| ------- | ----------- |
| `runscript <script name>` | Executes the given nwscript. Example: `runscript dm_killallplayers`
| `eval <script chunk>` | Executes the given nwscript chunk. Example: `eval ApplyEffectToObject(DURATION_TYPE_INSTANT, EffectDeath(), GetFirstPC());`
| `evalx <script chunk>` | Executes the given nwscript chunk, this command already includes all nwnx headers available in the module. Example: `evalx NWNX_Administration_ShutdownServer();`
| `loglevel <plugin> [<loglevel>]` | Sets the log level of the given plugin. `<plugin>` should not have the `NWNX_` prefix.  Example: `loglevel Events 7`
| `logformat [timestamp\|notimestamp] [plugin\|noplugin] [source\|nosource] [color\|nocolor] [force\|noforce] [text\|json]` | Control the output format of logs. Example: `logformat color timestamp noplugin nosource`
| `logrotate nwnx\|engine\|server\|all` | Moves `LOG_FILE_PATH` (`nwnx`), `nwengineLog.txt` (`engine`), the current `nwserverLog<N>.txt` (`server`), or all of them to `<name>.<UTC time>`, and logging continues in a new file of the same name, which starts by naming the archive (`LOG_FILE_PATH` in its banner, `rotated_from` in JSON; a server log with the engine's banner, the engine log with a dated line). No line is lost or written twice.
| `logreopen` | Reopens the log files that were moved away (`LOG_FILE_PATH`, `nwengineLog.txt`, `nwserverLog<N>.txt`), for external rotation. `NWNX_CORE_LOG_REOPEN_SIGNAL` does the same on a signal.

## JSON Logs

With `NWNX_CORE_LOG_FORMAT=json`, every NWNX log line is one JSON object, in [pino](https://getpino.io) style:

```json
{"time":1763648400123,"level":35,"lvl":"notice","msg":"Loading NWNX.","src":"nwnx","plugin":"NWNX_Core","file":"NWNXCore.cpp","line":612}
```

- `time`: milliseconds since the Unix epoch. `level`: 20 debug, 30 info, 35 notice, 40 warn, 50 error, 60 fatal.
- `file` and `line` are left out when `NWNX_CORE_LOG_SOURCE=0`. Any other key is a field of that record; a field named like one of the keys above (or `cat`, `err`, `logger`, `truncated`) gets a leading `_`, with a warning the first time. A key given twice keeps its last value.
- Plugins add fields with `LOG_INFO_F("Text line %d", n).With("rows", n).Msg("stable message")`: the text line is unchanged, the JSON record gets `msg` and the fields. A filtered call evaluates none of its arguments.
- The startup banner, version checks and assertion failures become records too. A crash report stays plain text, since the crash handler must not allocate. Other lines printed to `stdout` and `stderr` become records with `src` `stdout` or `stderr` (`NWNX_CORE_LOG_CAPTURE_STDIO`); a line in the engine's console format (`NWN_ENABLE_DEBUG_SPEW`) gets its level and `cat`, and is dropped while ServerLogRedirector makes engine records itself, as is the engine's `!! Userdata log emitted from non-main thread discarded !!` notice (the record was kept). Load ServerLogRedirector for the engine's log as records.
- `NWNX_CORE_LOG_MESSAGE` subscribers get the same payload as in text mode.
- Writing is async (`NWNX_CORE_LOG_ASYNC`), and `time` is when the record was made. Lines are never split: on a pipe, such as a container's `stdout`, NWNX writes at most 4096 bytes at a time and only whole lines, so other writers to the pipe cannot land inside them. A fatal error or a crash writes everything still queued first.

### Logging from DotNET, SWIG and Ruby

`NWNX_Core.so` exports two C functions (declared in `Core/NWNXLog.h`) so other runtimes can write records into the same stream:

```c
void NWNX_Log_Emit(int syslogLevel, const char* src, const char* logger, const char* msg, const char* fieldsJson);
int  NWNX_Log_IsEnabled(int syslogLevel, const char* src, const char* logger);
```

- `syslogLevel` as in `LOG_LEVEL` (7 debug to 3 error; fatal is treated as error). `src` is `dotnet`, `swig` or `ruby`: the record gets that `src`, the plugin name `NWNX_DotNET`, `NWNX_SWIG` or `NWNX_Ruby`, and that plugin's `LOG_LEVEL`. Any other `src` logs as `bridge` under Core's level.
- `logger` (optional) becomes the `logger` key; `NWNX_LOG_LOGGER_LEVELS` can give it its own level. `fieldsJson` is a JSON object whose members become fields, or `NULL`; JSON that does not parse or nests deeper than 64 levels adds a `fields_error` field instead, and any other value goes under `data`.
- Strings are UTF-8 and every pointer may be `NULL`. Text lines get the message and string fields in game-locale bytes (`NWNX_CORE_LOCALE`) like the rest of the line; JSON keeps them as UTF-8 whatever `NWNX_CORE_LOG_JSON_ENCODING` says. Both functions can be called from any thread and never throw. Called on the main thread while a script runs, the record gets the script context (`NWNX_CORE_LOG_CONTEXT`). As for `NWNX_Log` records, text lines show the fields and JSON has no `file`/`line`.
- `NWNX_Log_IsEnabled` lets a caller skip building the message and fields of a record that would be filtered.

DotNET and Anvil (C# 11, .NET 7 or later):
```csharp
var emit = (delegate* unmanaged<int, byte*, byte*, byte*, byte*, void>)
    NativeLibrary.GetExport(NativeLibrary.GetMainProgramHandle(), "NWNX_Log_Emit");
fixed (byte* src = "dotnet\0"u8, logger = "Bank\0"u8, msg = "deposit\0"u8, fields = "{\"gold\":250}\0"u8)
    emit(6, src, logger, msg, fields);
```

Code using the SWIG bindings (`NWN.Native`) calls it the same way, with `src` `swig`.

Ruby (`fiddle`):
```ruby
emit = Fiddle::Function.new(Fiddle::Handle::DEFAULT["NWNX_Log_Emit"], [Fiddle::TYPE_INT] + [Fiddle::TYPE_VOIDP] * 4, Fiddle::TYPE_VOID)
emit.call(6, "ruby", "bank", "deposit", '{"gold":250}')
```

## Custom Resman Definition File

A file that lets you specify additional resource directories that act the same as the `/development` and `/nwnx` folders.

Each line in the file must contain the following: `ALIASNAME /full/path/to/folder <priority>`

Example:

```
TEST /home/nwnx/test 70000001
STUFF /home/nwnx/stuff 70000002
```

## Plugin Management

### Environment Variable Configuration

Configuration of NWNX plugins are controlled by environment variables exported prior to starting the ``nwserver`` binary.  Typical usage would 
be in the script used to start the server.

Basic control of the plugin includes enabling/disabling the plugin and setting the logging level of the plugin.  The following format is used for
all plugins:

* NWNX_\<x>_LOG_LEVEL
* NWNX_\<x>_SKIP

### Plugin Loading

Inclusion or exclusion of the plugin at server startup time is contolled by the environment variable ``NWNX_<x>_SKIP`` where \<x> is the plugin name (all caps).

* Setting this to ``N`` will load the plugin (i.e. it will not be skipped) at startup (_default_).
* Setting this to ``Y`` will cause the plugin to _BE SKIPPED_ at startup.

__Example__
```bash
# Load the sql plugin at startup
export NWNX_SQL_SKIP=N
```

### Plugin Log Level

The default plugin log level is the same as the overall Core logging level.

It can be changed, however, by assinging a value to ``NWNX_<x>_LOG_LEVEL`` (where \<x> is the plugin name in all caps) in the same manner as the Core logging level.
See the Core documentation for a description of available logging levels.

__Example__
```bash
# Set the sql plugin to warning and above (warning, error, and fatal only)
export NWNX_SQL_LOG_LEVEL=4
```
