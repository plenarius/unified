#pragma once

#include "External/tinyformat/tinyformat.hpp"
#include "External/json/json.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <vector>
#include <ctime>

struct CExoString;
struct CResRef;

namespace NWNXLib::Log {

// The level check happens here, so a filtered call costs one load and evaluates none of its arguments.
#define LOG_IMPL(sev, format, ...) \
    (__builtin_expect(static_cast<uint32_t>(::NWNXLib::Log::Channel::sev) <= \
                      ::NWNXLib::Log::PluginLogLevel().load(std::memory_order_relaxed), 0) \
        ? ::NWNXLib::Log::TraceUnchecked(::NWNXLib::Log::Channel::sev, PLUGIN_NAME, __FILE__, __LINE__, (format), ##__VA_ARGS__) \
        : void())

// LOG_IMPL with fields: LOG_INFO_F("fmt", args).With("key", value).Msg("stable msg"). A filtered call evaluates
// neither the format arguments nor anything in the With() chain.
#define LOG_IMPL_F(sev, format, ...) \
    !__builtin_expect(static_cast<uint32_t>(::NWNXLib::Log::Channel::sev) <= \
                      ::NWNXLib::Log::PluginLogLevel().load(std::memory_order_relaxed), 0) \
        ? void() \
        : ::NWNXLib::Log::Voidify() & ::NWNXLib::Log::FormattedRecord(::NWNXLib::Log::Channel::sev, PLUGIN_NAME, \
                                                                      __FILE__, __LINE__, (format), ##__VA_ARGS__)

struct Channel
{
    enum Enum
    {
        // N.B.: Do not change the enum consts. They map to syslog severities.

        // Debug messages are mainly of use to the developer of a plugin.
        // They should be excessively verbose and should be turned off on production.
        SEV_DEBUG = 7,

        // Info messages are "informational". They contain operational information
        // that will be useful to server admins to see what is happening and can
        // be used to trace events after the fact.
        // These should be very low-volume.
        // This is the default log level for all sources.
        SEV_INFO = 6,

        // This is very similar to INFO, but implies that you want to *notice*
        // the event. This usually means aggregating logs somewhere and mailing
        // them out to you in a timely manner.
        SEV_NOTICE = 5,

        // Warnings are operational issues with the server, the codebase, or
        // the data that could potentially result in ERROR later on, but do not
        // impact the system yet in such a way that users would notice.
        SEV_WARNING = 4,

        // Errors imply user inpact or data loss; for example non-idempotent
        // operations failing. Errors can be recovered from but impact the user.
        SEV_ERROR = 3,

        // Fatals are unrecoverable errors. Use this log message when you want
        // to stop the world. Log messages to FATAL will never return.
        SEV_FATAL = 2
    };
};

template <typename ... Args>
void Trace(Channel::Enum channel, const char* plugin, const char* file, int line, const char* format, Args&& ... args);

Channel::Enum GetLogLevel(const char* plugin);
void SetLogLevel(const char* plugin, Channel::Enum logLevel);
// The atomic behind Get/SetLogLevel. It is never freed, so callers can keep the reference.
std::atomic<uint32_t>& GetLogLevelSlot(const char* plugin);
void SetPrintTimestamp(bool value);
bool GetPrintTimestamp();
void SetPrintDate(bool value);
bool GetPrintDate();
void SetPrintPlugin(bool value);
bool GetPrintPlugin();
void SetPrintSource(bool value);
bool GetPrintSource();
void SetColorOutput(bool value);
bool GetColorOutput();
void SetForceColor(bool value);
bool GetForceColor();
void SetLogFile(const std::string& logfile = "");

enum class Format : uint8_t { Text, Json };
namespace Json { struct Options; }
// The stdout format.
void SetFormat(Format format);
Format GetFormat();
void SetFileFormat(Format format);
Format GetFileFormat();
void SetJsonOptions(const Json::Options& options);
const Json::Options& GetJsonOptions();
// Members added to every JSON record, given as a JSON object. Returns an error message, or an empty string;
// renamed receives the reserved keys that were renamed with a leading '_'.
std::string SetJsonStatic(const std::string& object, std::vector<std::string>& renamed);

// Appends the fields of LOG_*_F records to text lines in logfmt (NWNX_CORE_LOG_TEXT_FIELDS).
void SetTextFields(bool value);
bool GetTextFields();

// Async writing: a writer thread does the I/O, so a slow stdout or disk does not stall the caller.
struct AsyncOptions
{
    uint32_t flushMs = 50;             // longest wait before a quiet line is written
    size_t maxBytes = 8 * 1024 * 1024; // queue size at which callers wait, or drop
    bool dropWhenFull = false;         // drop DEBUG, INFO and NOTICE records when full instead of waiting
};
// Starts the writer thread; records are queued from then on. FATAL records still drain and write at once.
void StartAsync(const AsyncOptions& options);
// Writes everything queued so far, on the calling thread.
void Flush();
// Writes what is queued and stops the writer; later records are written on the calling thread.
void Shutdown();
// For the crash handler: writes the queue without waiting for the lock, and writes later records directly.
void EmergencyFlush();

void InternalTrace(Channel::Enum channel, Channel::Enum allowedChannel, const char* message);
void WriteToLogFile(const char* message);

namespace {
// Internal linkage on purpose: with external linkage the dynamic linker would merge this function and its
// static across plugins, and every plugin would read the level of whichever loaded first.
inline std::atomic<uint32_t>& PluginLogLevel()
{
    static std::atomic<uint32_t>& s_level = GetLogLevelSlot(PLUGIN_NAME);
    return s_level;
}
}

// Field values that need more than their C++ type says.
struct Game { std::string_view s; };    // game-locale bytes: always transcoded
struct Utf8 { std::string_view s; };    // UTF-8: invalid sequences become U+FFFD
struct Oid { uint32_t id; };            // "8000012a", as ObjectToString prints it
struct RawJson { std::string_view s; }; // a value that is already valid JSON, copied as is

class Record;
// Adds context fields (the running script) to each record before its own fields; set by the Log plugin.
// It runs on whichever thread logs, and only when the record has fields.
using ContextSource = void (*)(Record& record);
void SetContextSource(ContextSource source);

// One log record, built in reusable thread-local buffers. A record started while another is being built on
// the same thread (from an argument or a log subscriber that logs itself) gets buffers of its own.
class Record
{
public:
    Record(Channel::Enum channel, const char* plugin, const char* file, int line);
    ~Record();
    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    // The message, as the text format prints it.
    std::ostream& Stream() { return *m_stream; }

    // Whether fields are encoded at all: false in text mode without NWNX_CORE_LOG_TEXT_FIELDS, where With()
    // does nothing. Lets a caller skip building a value no sink would show.
    bool HasFields() const { return m_fields; }

    // Fields, in call order. A key used twice keeps its last value; a reserved key (msg, level, ...) is
    // renamed with a leading '_'. Strings follow NWNX_CORE_LOG_JSON_ENCODING unless wrapped in Game or Utf8.
    Record& With(std::string_view key, std::string_view value) { return m_fields ? AddString(key, value, StringKind::Default) : *this; }
    Record& With(std::string_view key, const char* value) { return With(key, std::string_view(value ? value : "")); }
    Record& With(std::string_view key, const std::string& value) { return With(key, std::string_view(value)); }
    Record& With(std::string_view key, const CExoString& value);
    Record& With(std::string_view key, const CResRef& value);
    Record& With(std::string_view key, Game value) { return m_fields ? AddString(key, value.s, StringKind::Game) : *this; }
    Record& With(std::string_view key, Utf8 value) { return m_fields ? AddString(key, value.s, StringKind::Utf8) : *this; }
    Record& With(std::string_view key, Oid value) { return m_fields ? AddOid(key, value.id) : *this; }
    Record& With(std::string_view key, RawJson value) { return m_fields ? AddRaw(key, value.s) : *this; }
    Record& With(std::string_view key, bool value) { return m_fields ? AddRaw(key, value ? "true" : "false") : *this; }
    Record& With(std::string_view key, double value) { return m_fields ? AddDouble(key, value) : *this; }
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Record& With(std::string_view key, T value)
    {
        if (!m_fields)
            return *this;
        if constexpr (std::is_signed_v<T>)
            return AddInt(key, static_cast<int64_t>(value));
        else
            return AddUInt(key, static_cast<uint64_t>(value));
    }
    // Objects and arrays; strings in it follow the same encoding rules as other string values.
    Record& With(std::string_view key, const nlohmann::json& value) { return m_fields ? AddJson(key, value) : *this; }
    // Left out when empty.
    template <typename T>
    Record& With(std::string_view key, const std::optional<T>& value) { return value ? With(key, *value) : *this; }
    // A Platform::GetStackTrace text as an array of frames.
    Record& WithStack(std::string_view key, std::string_view trace) { return m_fields ? AddStack(key, trace) : *this; }
    // "err":{"message":...,"code":...}
    Record& Err(std::string_view message, std::optional<int64_t> code = {}) { return m_fields ? AddErr(message, code) : *this; }

    // The JSON msg, a short stable text such as "sql query"; text lines keep the formatted message.
    Record& Msg(std::string_view msg);
    // JSON "src" ("nwnx" unless set); the string must outlive the record, e.g. a literal.
    Record& Src(const char* src) { m_src = src; return *this; }
    // A reserved origin key written after plugin/file/line, e.g. "logger" or "cat", and in text before the
    // fields when they show. The key must be a literal.
    Record& Origin(const char* key, std::string_view value);
    // Shows this record's fields in text lines whatever NWNX_CORE_LOG_TEXT_FIELDS says, for output that has
    // no legacy form to keep. Call it before the first With().
    Record& TextFields();

    // Writes only to the text sinks, for a legacy line whose content JSON has in another record. Call it first.
    Record& TextOnly();

    // Writes to every sink. FATAL then asserts and aborts.
    void Write();
    // Writes only to the sinks that use JSON, for reports that print their own text.
    void WriteJson();

    struct Buffers;

protected:
    // Set by FormattedRecord: the message is formatted only if a sink or subscriber needs it.
    const char* m_format = nullptr;
    const tfm::FormatList* m_args = nullptr;

private:
    enum class StringKind : uint8_t { Default, Game, Utf8 };
    Record& AddString(std::string_view key, std::string_view value, StringKind kind);
    Record& AddOid(std::string_view key, uint32_t id);
    Record& AddRaw(std::string_view key, std::string_view json);
    Record& AddInt(std::string_view key, int64_t value);
    Record& AddUInt(std::string_view key, uint64_t value);
    Record& AddDouble(std::string_view key, double value);
    Record& AddJson(std::string_view key, const nlohmann::json& value);
    Record& AddStack(std::string_view key, std::string_view trace);
    Record& AddErr(std::string_view message, std::optional<int64_t> code);
    // Starts a member: drops an earlier one with the same key, writes ,"key": and returns the buffer.
    std::string& BeginField(std::string_view key, bool reservedAllowed = false);
    // Keeps the text form of the field just begun.
    void SetRaw(std::string_view value);
    void AddContext();
    void Format();

    Buffers* m_buffers;
    std::ostream* m_stream;
    const char* m_plugin;
    const char* m_file;
    const char* m_src = "nwnx";
    int m_line;
    Channel::Enum m_channel;
    bool m_nested;
    bool m_fields;
    bool m_json;
    bool m_textRaw;  // a text sink shows this record's fields: keep strings' own bytes
    bool m_textOnly = false;
};

// The record LOG_*_F builds: it keeps references to the format arguments, which live until the end of the
// full expression, formats only when needed and writes when the expression ends.
template <typename... Args>
class FormattedRecord final : public Record
{
public:
    FormattedRecord(Channel::Enum channel, const char* plugin, const char* file, int line, const char* format, const Args&... args)
        : Record(channel, plugin, file, line), m_list(args...)
    {
        m_format = format;
        m_args = &m_list;
    }
    ~FormattedRecord() { Write(); }

private:
    tfm::detail::FormatListN<sizeof...(Args)> m_list;
};
template <typename... Args>
FormattedRecord(Channel::Enum, const char*, const char*, int, const char*, const Args&...) -> FormattedRecord<Args...>;

// Lets LOG_IMPL_F be one expression: & binds looser than the With() chain and tighter than ?:.
struct Voidify
{
    void operator&(const Record&) const {}
};

// Formats and writes a line whose level was already checked.
template <typename ... Args>
void TraceUnchecked(Channel::Enum channel, const char* plugin, const char* file, int line, const char* format, Args&& ... args)
{
    Record record(channel, plugin, file, line);
    tfm::format(record.Stream(), format, std::forward<Args>(args)...);
    record.Write();
}

template <typename ... Args>
void Trace(Channel::Enum channel, const char* plugin, const char* file, int line, const char* format, Args&& ... args)
{
    Channel::Enum allowedChannel = GetLogLevel(plugin);

    if (channel > allowedChannel)
    {
        // No need to do anything. Our log level is lower.
        return;
    }

    TraceUnchecked(channel, plugin, file, line, format, std::forward<Args>(args)...);
}

}

std::ostream& operator<<(std::ostream& out, const CExoString& str);
std::ostream& operator<<(std::ostream& out, const CResRef& str);
