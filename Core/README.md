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
| `NWNX_CORE_LOG_JSON_LEVEL` | number, label | number | `number`: `"level":30,"lvl":"info"`. `label`: `"level":"info"`.
| `NWNX_CORE_LOG_JSON_ENCODING` | auto, locale, utf8 | auto | How strings become UTF-8. `auto`: valid UTF-8 is kept, anything else is converted from `NWNX_CORE_LOCALE`. `locale`: always converted. `utf8`: invalid bytes become U+FFFD.
| `NWNX_CORE_LOG_JSON_STRIP_COLORS` | 0-1 | 1 | Removes NWN colour tokens (`<cXYZ>`, `</c>`) from JSON strings.
| `NWNX_CORE_LOG_JSON_MAX_VALUE` | int | 8192 | Longer strings are cut (at a character boundary) and the record gets `"truncated":true`.
| `NWNX_CORE_LOG_JSON_MAX_LINE` | int | 65536 | Longer records are shortened the same way: the message first, then the fields are dropped.
| `NWNX_CORE_LOG_JSON_STATIC` | JSON object | Unset | Added to every JSON record, e.g. `{"server":"main"}`.
| `NWNX_CORE_LOG_ASYNC` | auto, true, false | auto | Write logs on a background thread, so a slow `stdout` or disk does not stall the server. `auto`: on when `LOG_FORMAT` or `LOG_FILE_FORMAT` is `json`. Decided at startup: switching to JSON later with `logformat` stays synchronous. The bytes are the same, but NWNX lines can reach `stdout` up to `LOG_ASYNC_FLUSH_MS` after other output.
| `NWNX_CORE_LOG_ASYNC_FLUSH_MS` | int | 50 | The longest a line waits before it is written. Warnings and worse, and every 64 KiB of lines, are written at once.
| `NWNX_CORE_LOG_ASYNC_MAX_BYTES` | int | 8388608 | How much may wait to be written (at least 262144).
| `NWNX_CORE_LOG_ASYNC_FULL` | block, drop | block | When that is full: `block` waits for room; `drop` drops debug, info and notice lines and later logs a warning with the number dropped. Warnings and worse always wait.
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
| `logrotate engine\|server` | Moves `nwengineLog.txt` (`engine`) or the current `nwserverLog<N>.txt` (`server`) to `<name>.<UTC time>`; the engine continues in a new file of the same name, which starts by naming the archive (a server log with the engine's banner, the engine log with a dated line). No line is lost or written twice.

## JSON Logs

With `NWNX_CORE_LOG_FORMAT=json`, every NWNX log line is one JSON object, in [pino](https://getpino.io) style:

```json
{"time":1763648400123,"level":35,"lvl":"notice","msg":"Loading NWNX.","src":"nwnx","plugin":"NWNX_Core","file":"NWNXCore.cpp","line":612}
```

- `time`: milliseconds since the Unix epoch. `level`: 20 debug, 30 info, 35 notice, 40 warn, 50 error, 60 fatal.
- `file` and `line` are left out when `NWNX_CORE_LOG_SOURCE=0`. Any other key is a field of that record; a field named like one of the keys above (or `cat`, `err`, `logger`, `truncated`) gets a leading `_`.
- The startup banner, version checks, assertion failures and crashes become records too. Engine output and other lines printed to `stdout` stay as they are.
- `NWNX_CORE_LOG_MESSAGE` subscribers get the same payload as in text mode.
- Writing is async (`NWNX_CORE_LOG_ASYNC`), and `time` is when the record was made. Lines are never split: on a pipe, such as a container's `stdout`, NWNX writes at most 4096 bytes at a time and only whole lines, so other writers to the pipe cannot land inside them. A fatal error or a crash writes everything still queued first.

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
