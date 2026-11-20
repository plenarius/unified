#pragma once

#include "External/tinyformat/tinyformat.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>
#include <vector>
#include <ctime>

namespace NWNXLib::Log {

// The level check happens here, so a filtered call costs one load and evaluates none of its arguments.
#define LOG_IMPL(sev, format, ...) \
    (__builtin_expect(static_cast<uint32_t>(::NWNXLib::Log::Channel::sev) <= \
                      ::NWNXLib::Log::PluginLogLevel().load(std::memory_order_relaxed), 0) \
        ? ::NWNXLib::Log::TraceUnchecked(::NWNXLib::Log::Channel::sev, PLUGIN_NAME, __FILE__, __LINE__, (format), ##__VA_ARGS__) \
        : void())

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

    // Fields for the JSON sinks; keys are string literals.
    Record& With(const char* key, std::string_view value);
    Record& With(const char* key, int64_t value);
    // A Platform::GetStackTrace text as an array of frames.
    Record& WithStack(const char* key, std::string_view trace);

    // Writes to every sink. FATAL then asserts and aborts.
    void Write();
    // Writes only to the sinks that use JSON, for reports that print their own text.
    void WriteJson();

    struct Buffers;

private:
    Buffers* m_buffers;
    std::ostream* m_stream;
    const char* m_plugin;
    const char* m_file;
    int m_line;
    Channel::Enum m_channel;
    bool m_nested;
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

struct CExoString;
struct CResRef;

std::ostream& operator<<(std::ostream& out, const CExoString& str);
std::ostream& operator<<(std::ostream& out, const CResRef& str);
